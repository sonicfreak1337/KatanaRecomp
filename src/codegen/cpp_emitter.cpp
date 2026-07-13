#include "katana/codegen/cpp_emitter.hpp"

#include "katana/ir/ir.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace katana::codegen {
namespace {

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;

    output
        << "0x"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << value
        << "u";

    return output.str();
}

std::string function_name(const std::uint32_t address) {
    std::ostringstream output;

    output
        << "fn_"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << address;

    return output.str();
}

std::uint32_t fallthrough_address(
    const katana::ir::Instruction& instruction
) {
    return instruction.source_address +
        (instruction.has_delay_slot ? 4u : 2u);
}

void emit_indent(
    std::ostringstream& output,
    const int level
) {
    for (int index = 0; index < level; ++index) {
        output << "    ";
    }
}

void emit_simple_instruction(
    std::ostringstream& output,
    const katana::ir::Instruction& instruction,
    const int indent
) {
    using Operation = katana::ir::Operation;

    emit_indent(output, indent);

    switch (instruction.operation) {
        case Operation::Nop:
            output << "/* nop */\n";
            return;

        case Operation::MovImmediate:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = static_cast<std::uint32_t>("
                << instruction.immediate
                << ");\n";
            return;

        case Operation::AddImmediate:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] += static_cast<std::uint32_t>("
                << instruction.immediate
                << ");\n";
            return;

        case Operation::MovRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::AddRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] += cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::SubRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] -= cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::NegateRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = 0u - cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::NotRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = ~cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;
        case Operation::AddWithCarry:
            output
                << "{\n"
                << "const std::uint64_t carry_in = cpu.t ? 1ull : 0ull;\n"
                << "const std::uint64_t result =\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "]) +\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) +\n"
                << "    carry_in;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = static_cast<std::uint32_t>(result);\n"
                << "cpu.t = result > 0xFFFFFFFFull;\n"
                << "}\n";
            return;

        case Operation::AddWithOverflow:
            output
                << "{\n"
                << "const std::uint32_t lhs = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "const std::uint32_t rhs = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t result = lhs + rhs;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = result;\n"
                << "cpu.t =\n"
                << "    ((~(lhs ^ rhs) & (lhs ^ result)) & "
                << "0x80000000u) != 0u;\n"
                << "}\n";
            return;

        case Operation::SubWithCarry:
            output
                << "{\n"
                << "const std::uint64_t borrow_in = cpu.t ? 1ull : 0ull;\n"
                << "const std::uint64_t minuend =\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "]);\n"
                << "const std::uint64_t subtrahend =\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) + borrow_in;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = static_cast<std::uint32_t>(\n"
                << "    minuend - subtrahend\n"
                << ");\n"
                << "cpu.t = minuend < subtrahend;\n"
                << "}\n";
            return;

        case Operation::SubWithOverflow:
            output
                << "{\n"
                << "const std::uint32_t lhs = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "const std::uint32_t rhs = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t result = lhs - rhs;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = result;\n"
                << "cpu.t =\n"
                << "    (((lhs ^ rhs) & (lhs ^ result)) & "
                << "0x80000000u) != 0u;\n"
                << "}\n";
            return;

        case Operation::NegateWithCarry:
            output
                << "{\n"
                << "const std::uint64_t borrow_in = cpu.t ? 1ull : 0ull;\n"
                << "const std::uint64_t subtrahend =\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) + borrow_in;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = static_cast<std::uint32_t>(\n"
                << "    0ull - subtrahend\n"
                << ");\n"
                << "cpu.t = subtrahend != 0ull;\n"
                << "}\n";
            return;
        case Operation::ExtendUnsignedByte:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] & 0x000000FFu;\n";
            return;

        case Operation::ExtendUnsignedWord:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] & 0x0000FFFFu;\n";
            return;

        case Operation::ExtendSignedByte:
            output
                << "{\n"
                << "std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] & 0x000000FFu;\n"
                << "if ((value & 0x00000080u) != 0u) {\n"
                << "    value |= 0xFFFFFF00u;\n"
                << "}\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value;\n"
                << "}\n";
            return;

        case Operation::ExtendSignedWord:
            output
                << "{\n"
                << "std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] & 0x0000FFFFu;\n"
                << "if ((value & 0x00008000u) != 0u) {\n"
                << "    value |= 0xFFFF0000u;\n"
                << "}\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value;\n"
                << "}\n";
            return;

        case Operation::SwapBytes:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] =\n"
                << "    (value & 0xFFFF0000u) |\n"
                << "    ((value & 0x000000FFu) << 8u) |\n"
                << "    ((value & 0x0000FF00u) >> 8u);\n"
                << "}\n";
            return;

        case Operation::SwapWords:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = (value << 16u) | (value >> 16u);\n"
                << "}\n";
            return;

        case Operation::ExtractMiddle:
            output
                << "{\n"
                << "const std::uint32_t source = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t destination = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] =\n"
                << "    (source << 16u) |\n"
                << "    (destination >> 16u);\n"
                << "}\n";
            return;
        case Operation::DecrementAndTest:
            output
                << "{\n"
                << "const std::uint32_t result = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] - 1u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = result;\n"
                << "cpu.t = result == 0u;\n"
                << "}\n";
            return;

        case Operation::MoveT:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.t ? 1u : 0u;\n";
            return;
        case Operation::ShiftLogicalLeftOne:
        case Operation::ShiftArithmeticLeftOne:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "cpu.t = (value & 0x80000000u) != 0u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value << 1u;\n"
                << "}\n";
            return;

        case Operation::ShiftLogicalRightOne:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "cpu.t = (value & 0x00000001u) != 0u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value >> 1u;\n"
                << "}\n";
            return;

        case Operation::ShiftArithmeticRightOne:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "cpu.t = (value & 0x00000001u) != 0u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] =\n"
                << "    (value >> 1u) |\n"
                << "    (value & 0x80000000u);\n"
                << "}\n";
            return;
        case Operation::ShiftLogicalLeftTwo:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] <<= 2u;\n";
            return;

        case Operation::ShiftLogicalLeftEight:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] <<= 8u;\n";
            return;

        case Operation::ShiftLogicalLeftSixteen:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] <<= 16u;\n";
            return;

        case Operation::ShiftLogicalRightTwo:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] >>= 2u;\n";
            return;

        case Operation::ShiftLogicalRightEight:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] >>= 8u;\n";
            return;

        case Operation::ShiftLogicalRightSixteen:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] >>= 16u;\n";
            return;
        case Operation::RotateLeft:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "cpu.t = (value & 0x80000000u) != 0u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = (value << 1u) | (value >> 31u);\n"
                << "}\n";
            return;

        case Operation::RotateRight:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "cpu.t = (value & 0x00000001u) != 0u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = (value >> 1u) | (value << 31u);\n"
                << "}\n";
            return;

        case Operation::RotateLeftThroughT:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "const bool old_t = cpu.t;\n"
                << "cpu.t = (value & 0x80000000u) != 0u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = (value << 1u) | (old_t ? 1u : 0u);\n"
                << "}\n";
            return;

        case Operation::RotateRightThroughT:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "const bool old_t = cpu.t;\n"
                << "cpu.t = (value & 0x00000001u) != 0u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] =\n"
                << "    (value >> 1u) |\n"
                << "    (old_t ? 0x80000000u : 0u);\n"
                << "}\n";
            return;
        case Operation::AndRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] &= cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::OrRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] |= cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::XorRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] ^= cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::AndImmediate:
            output
                << "cpu.r[0] &= static_cast<std::uint32_t>("
                << instruction.immediate
                << ");\n";
            return;

        case Operation::OrImmediate:
            output
                << "cpu.r[0] |= static_cast<std::uint32_t>("
                << instruction.immediate
                << ");\n";
            return;

        case Operation::XorImmediate:
            output
                << "cpu.r[0] ^= static_cast<std::uint32_t>("
                << instruction.immediate
                << ");\n";
            return;
        case Operation::ClearT:
            output << "cpu.t = false;\n";
            return;

        case Operation::SetT:
            output << "cpu.t = true;\n";
            return;

        case Operation::CompareEqualImmediate:
            output
                << "cpu.t = cpu.r[0] == static_cast<std::uint32_t>("
                << instruction.immediate
                << ");\n";
            return;

        case Operation::CompareEqualRegister:
            output
                << "cpu.t = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] == cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::CompareHigherOrSame:
            output
                << "cpu.t = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] >= cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::CompareGreaterOrEqual:
            output
                << "cpu.t = (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] ^ 0x80000000u) >= (cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] ^ 0x80000000u);\n";
            return;

        case Operation::CompareHigher:
            output
                << "cpu.t = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] > cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n";
            return;

        case Operation::CompareGreaterThan:
            output
                << "cpu.t = (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] ^ 0x80000000u) > (cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] ^ 0x80000000u);\n";
            return;

        case Operation::ComparePositiveOrZero:
            output
                << "cpu.t = (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] & 0x80000000u) == 0u;\n";
            return;

        case Operation::ComparePositive:
            output
                << "cpu.t = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] != 0u && (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] & 0x80000000u) == 0u;\n";
            return;

        case Operation::CompareString:
            output
                << "cpu.t =\n"
                << "    (((cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] ^ cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) & 0x000000FFu) == 0u) ||\n"
                << "    (((cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] ^ cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) & 0x0000FF00u) == 0u) ||\n"
                << "    (((cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] ^ cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) & 0x00FF0000u) == 0u) ||\n"
                << "    (((cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] ^ cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) & 0xFF000000u) == 0u);\n";
            return;
        case Operation::TestImmediate:
            output
                << "cpu.t = (cpu.r[0] & static_cast<std::uint32_t>("
                << instruction.immediate
                << ")) == 0u;\n";
            return;

        case Operation::TestRegister:
            output
                << "cpu.t = (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] & cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]) == 0u;\n";
            return;
        case Operation::LoadByteSigned:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_s8(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::LoadWordSigned:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_s16(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::LoadLong:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_u32(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::StoreByte:
            output
                << "cpu.memory.write_u8(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "], static_cast<std::uint8_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]));\n";
            return;

        case Operation::StoreWord:
            output
                << "cpu.memory.write_u16(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "], static_cast<std::uint16_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]));\n";
            return;

        case Operation::StoreLong:
            output
                << "cpu.memory.write_u32(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "], cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::Unknown:
            output
                << "throw std::runtime_error("
                << "\"Unbekannte IR-Instruktion bei "
                << hex32(instruction.source_address)
                << "\");\n";
            return;

        case Operation::Branch:
        case Operation::Call:
        case Operation::BranchIfTrue:
        case Operation::BranchIfFalse:
        case Operation::JumpRegister:
        case Operation::CallRegister:
        case Operation::Return:
            output
                << "throw std::runtime_error("
                << "\"Kontrollfluss im Delay Slot wird nicht unterstuetzt\");\n";
            return;
    }
}

void emit_direct_call(
    std::ostringstream& output,
    const std::uint32_t target,
    const std::unordered_set<std::uint32_t>& known_functions,
    const int indent
) {
    emit_indent(output, indent);
    output << "cpu.pc = " << hex32(target) << ";\n";

    emit_indent(output, indent);

    if (known_functions.contains(target)) {
        output
            << function_name(target)
            << "(cpu);\n";
    } else {
        output
            << "unresolved_call(cpu, "
            << hex32(target)
            << ");\n";
    }
}

void emit_terminal(
    std::ostringstream& output,
    const katana::ir::BasicBlock& block,
    const std::size_t control_index,
    const std::unordered_set<std::uint32_t>& known_functions,
    const int indent
) {
    using Operation = katana::ir::Operation;

    const auto& instruction =
        block.instructions[control_index];

    const katana::ir::Instruction* delay_slot = nullptr;

    if (
        instruction.has_delay_slot &&
        control_index + 1u < block.instructions.size() &&
        block.instructions[control_index + 1u].is_delay_slot
    ) {
        delay_slot =
            &block.instructions[control_index + 1u];
    }

    switch (instruction.operation) {
        case Operation::Branch:
            if (!instruction.target_address.has_value()) {
                throw std::runtime_error(
                    "Direkter IR-Sprung besitzt kein Ziel."
                );
            }

            if (delay_slot != nullptr) {
                emit_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );
            }

            emit_indent(output, indent);
            output
                << "cpu.pc = "
                << hex32(*instruction.target_address)
                << ";\n";

            emit_indent(output, indent);
            output << "continue;\n";
            return;

        case Operation::Call:
            if (!instruction.target_address.has_value()) {
                throw std::runtime_error(
                    "Direkter IR-Aufruf besitzt kein Ziel."
                );
            }

            emit_indent(output, indent);
            output
                << "cpu.pr = "
                << hex32(instruction.source_address + 4u)
                << ";\n";

            if (delay_slot != nullptr) {
                emit_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );
            }

            emit_direct_call(
                output,
                *instruction.target_address,
                known_functions,
                indent
            );

            emit_indent(output, indent);
            output
                << "cpu.pc = "
                << hex32(fallthrough_address(instruction))
                << ";\n";

            emit_indent(output, indent);
            output << "continue;\n";
            return;

        case Operation::BranchIfTrue:
        case Operation::BranchIfFalse: {
            if (!instruction.target_address.has_value()) {
                throw std::runtime_error(
                    "Bedingter IR-Sprung besitzt kein Ziel."
                );
            }

            const auto condition =
                instruction.operation ==
                    Operation::BranchIfTrue
                    ? "cpu.t"
                    : "!cpu.t";

            if (delay_slot != nullptr) {
                emit_indent(output, indent);
                output
                    << "const bool take_branch = "
                    << condition
                    << ";\n";

                emit_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );

                emit_indent(output, indent);
                output
                    << "cpu.pc = take_branch ? "
                    << hex32(*instruction.target_address)
                    << " : "
                    << hex32(fallthrough_address(instruction))
                    << ";\n";
            } else {
                emit_indent(output, indent);
                output
                    << "cpu.pc = "
                    << condition
                    << " ? "
                    << hex32(*instruction.target_address)
                    << " : "
                    << hex32(fallthrough_address(instruction))
                    << ";\n";
            }

            emit_indent(output, indent);
            output << "continue;\n";
            return;
        }

        case Operation::JumpRegister:
            emit_indent(output, indent);
            output
                << "const std::uint32_t jump_target = cpu.r["
                << static_cast<unsigned>(
                    instruction.branch_register
                )
                << "];\n";

            if (delay_slot != nullptr) {
                emit_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );
            }

            emit_indent(output, indent);
            output << "unresolved_jump(cpu, jump_target);\n";

            emit_indent(output, indent);
            output << "return;\n";
            return;

        case Operation::CallRegister:
            emit_indent(output, indent);
            output
                << "const std::uint32_t call_target = cpu.r["
                << static_cast<unsigned>(
                    instruction.branch_register
                )
                << "];\n";

            emit_indent(output, indent);
            output
                << "cpu.pr = "
                << hex32(instruction.source_address + 4u)
                << ";\n";

            if (delay_slot != nullptr) {
                emit_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );
            }

            emit_indent(output, indent);
            output
                << "unresolved_call(cpu, call_target);\n";

            emit_indent(output, indent);
            output
                << "cpu.pc = "
                << hex32(fallthrough_address(instruction))
                << ";\n";

            emit_indent(output, indent);
            output << "continue;\n";
            return;

        case Operation::Return:
            if (delay_slot != nullptr) {
                emit_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );
            }

            emit_indent(output, indent);
            output << "cpu.pc = cpu.pr;\n";

            emit_indent(output, indent);
            output << "return;\n";
            return;

        case Operation::Unknown:
        case Operation::Nop:
        case Operation::MovImmediate:
        case Operation::AddImmediate:
        case Operation::MovRegister:
        case Operation::AddRegister:
        case Operation::SubRegister:
        case Operation::NegateRegister:
        case Operation::NotRegister:
        case Operation::AddWithCarry:
        case Operation::AddWithOverflow:
        case Operation::SubWithCarry:
        case Operation::SubWithOverflow:
        case Operation::NegateWithCarry:
        case Operation::ExtendUnsignedByte:
        case Operation::ExtendUnsignedWord:
        case Operation::ExtendSignedByte:
        case Operation::ExtendSignedWord:
        case Operation::SwapBytes:
        case Operation::SwapWords:
        case Operation::ExtractMiddle:
        case Operation::DecrementAndTest:
        case Operation::MoveT:
        case Operation::ShiftLogicalLeftOne:
        case Operation::ShiftLogicalRightOne:
        case Operation::ShiftArithmeticLeftOne:
        case Operation::ShiftArithmeticRightOne:
        case Operation::ShiftLogicalLeftTwo:
        case Operation::ShiftLogicalLeftEight:
        case Operation::ShiftLogicalLeftSixteen:
        case Operation::ShiftLogicalRightTwo:
        case Operation::ShiftLogicalRightEight:
        case Operation::ShiftLogicalRightSixteen:
        case Operation::RotateLeft:
        case Operation::RotateRight:
        case Operation::RotateLeftThroughT:
        case Operation::RotateRightThroughT:
        case Operation::AndRegister:
        case Operation::OrRegister:
        case Operation::XorRegister:
        case Operation::AndImmediate:
        case Operation::OrImmediate:
        case Operation::XorImmediate:
        case Operation::ClearT:
        case Operation::SetT:
        case Operation::CompareEqualImmediate:
        case Operation::CompareEqualRegister:
        case Operation::CompareHigherOrSame:
        case Operation::CompareGreaterOrEqual:
        case Operation::CompareHigher:
        case Operation::CompareGreaterThan:
        case Operation::ComparePositiveOrZero:
        case Operation::ComparePositive:
        case Operation::CompareString:
        case Operation::TestImmediate:
        case Operation::TestRegister:
        case Operation::LoadByteSigned:
        case Operation::LoadWordSigned:
        case Operation::LoadLong:
        case Operation::StoreByte:
        case Operation::StoreWord:
        case Operation::StoreLong:
            break;
    }

    throw std::runtime_error(
        "Eine nichtterminale IR-Instruktion wurde als Terminal behandelt."
    );
}

bool is_control_flow(
    const katana::ir::Operation operation
) {
    using Operation = katana::ir::Operation;

    switch (operation) {
        case Operation::Branch:
        case Operation::Call:
        case Operation::BranchIfTrue:
        case Operation::BranchIfFalse:
        case Operation::JumpRegister:
        case Operation::CallRegister:
        case Operation::Return:
            return true;

        case Operation::Unknown:
        case Operation::Nop:
        case Operation::MovImmediate:
        case Operation::AddImmediate:
        case Operation::MovRegister:
        case Operation::AddRegister:
        case Operation::SubRegister:
        case Operation::NegateRegister:
        case Operation::NotRegister:
        case Operation::AddWithCarry:
        case Operation::AddWithOverflow:
        case Operation::SubWithCarry:
        case Operation::SubWithOverflow:
        case Operation::NegateWithCarry:
        case Operation::ExtendUnsignedByte:
        case Operation::ExtendUnsignedWord:
        case Operation::ExtendSignedByte:
        case Operation::ExtendSignedWord:
        case Operation::SwapBytes:
        case Operation::SwapWords:
        case Operation::ExtractMiddle:
        case Operation::DecrementAndTest:
        case Operation::MoveT:
        case Operation::ShiftLogicalLeftOne:
        case Operation::ShiftLogicalRightOne:
        case Operation::ShiftArithmeticLeftOne:
        case Operation::ShiftArithmeticRightOne:
        case Operation::ShiftLogicalLeftTwo:
        case Operation::ShiftLogicalLeftEight:
        case Operation::ShiftLogicalLeftSixteen:
        case Operation::ShiftLogicalRightTwo:
        case Operation::ShiftLogicalRightEight:
        case Operation::ShiftLogicalRightSixteen:
        case Operation::RotateLeft:
        case Operation::RotateRight:
        case Operation::RotateLeftThroughT:
        case Operation::RotateRightThroughT:
        case Operation::AndRegister:
        case Operation::OrRegister:
        case Operation::XorRegister:
        case Operation::AndImmediate:
        case Operation::OrImmediate:
        case Operation::XorImmediate:
        case Operation::ClearT:
        case Operation::SetT:
        case Operation::CompareEqualImmediate:
        case Operation::CompareEqualRegister:
        case Operation::CompareHigherOrSame:
        case Operation::CompareGreaterOrEqual:
        case Operation::CompareHigher:
        case Operation::CompareGreaterThan:
        case Operation::ComparePositiveOrZero:
        case Operation::ComparePositive:
        case Operation::CompareString:
        case Operation::TestImmediate:
        case Operation::TestRegister:
        case Operation::LoadByteSigned:
        case Operation::LoadWordSigned:
        case Operation::LoadLong:
        case Operation::StoreByte:
        case Operation::StoreWord:
        case Operation::StoreLong:
            return false;
    }

    return false;
}

void emit_block(
    std::ostringstream& output,
    const katana::ir::BasicBlock& block,
    const std::unordered_set<std::uint32_t>& known_functions
) {
    output
        << "            case "
        << hex32(block.start_address)
        << ": {\n";

    std::optional<std::size_t> control_index;

    for (
        std::size_t index = 0;
        index < block.instructions.size();
        ++index
    ) {
        const auto& instruction =
            block.instructions[index];

        if (instruction.is_delay_slot) {
            continue;
        }

        if (is_control_flow(instruction.operation)) {
            control_index = index;
            break;
        }

        emit_simple_instruction(
            output,
            instruction,
            4
        );
    }

    if (control_index.has_value()) {
        emit_terminal(
            output,
            block,
            *control_index,
            known_functions,
            4
        );
    } else if (block.successors.size() == 1u) {
        emit_indent(output, 4);
        output
            << "cpu.pc = "
            << hex32(block.successors.front())
            << ";\n";

        emit_indent(output, 4);
        output << "continue;\n";
    } else if (block.successors.empty()) {
        emit_indent(output, 4);
        output << "return;\n";
    } else {
        emit_indent(output, 4);
        output
            << "throw std::runtime_error("
            << "\"Mehrdeutiger Block ohne Terminalinstruktion\");\n";
    }

    output << "            }\n";
}

}

std::string emit_cpp_program(
    const std::span<const katana::ir::Function> functions,
    const std::uint32_t entry_address
) {
    if (functions.empty()) {
        throw std::invalid_argument(
            "Es wurden keine IR-Funktionen uebergeben."
        );
    }

    std::unordered_set<std::uint32_t> known_functions;
    known_functions.reserve(functions.size());

    for (const auto& function : functions) {
        known_functions.insert(function.entry_address);
    }

    if (!known_functions.contains(entry_address)) {
        throw std::invalid_argument(
            "Die Einstiegsfunktion ist im IR-Programm nicht vorhanden."
        );
    }

    std::ostringstream output;

    output
        << "#include <array>\n"
        << "#include <cstddef>\n"
        << "#include <cstdint>\n"
        << "#include <stdexcept>\n"
        << "#include <vector>\n\n"
        << "namespace katana_generated {\n\n"
        << "class Memory {\n"
        << "public:\n"
        << "    explicit Memory(const std::size_t size = 1024u * 1024u)\n"
        << "        : bytes_(size, 0u) {}\n\n"
        << "    [[nodiscard]] std::uint8_t read_u8(\n"
        << "        const std::uint32_t address\n"
        << "    ) const {\n"
        << "        check(address, 1u);\n"
        << "        return bytes_[static_cast<std::size_t>(address)];\n"
        << "    }\n\n"
        << "    [[nodiscard]] std::uint16_t read_u16(\n"
        << "        const std::uint32_t address\n"
        << "    ) const {\n"
        << "        check(address, 2u);\n"
        << "        const auto offset = static_cast<std::size_t>(address);\n"
        << "        return static_cast<std::uint16_t>(\n"
        << "            static_cast<std::uint16_t>(bytes_[offset]) |\n"
        << "            (static_cast<std::uint16_t>(bytes_[offset + 1u]) << 8u)\n"
        << "        );\n"
        << "    }\n\n"
        << "    [[nodiscard]] std::uint32_t read_u32(\n"
        << "        const std::uint32_t address\n"
        << "    ) const {\n"
        << "        check(address, 4u);\n"
        << "        const auto offset = static_cast<std::size_t>(address);\n"
        << "        return\n"
        << "            static_cast<std::uint32_t>(bytes_[offset]) |\n"
        << "            (static_cast<std::uint32_t>(bytes_[offset + 1u]) << 8u) |\n"
        << "            (static_cast<std::uint32_t>(bytes_[offset + 2u]) << 16u) |\n"
        << "            (static_cast<std::uint32_t>(bytes_[offset + 3u]) << 24u);\n"
        << "    }\n\n"
        << "    [[nodiscard]] std::uint32_t read_s8(\n"
        << "        const std::uint32_t address\n"
        << "    ) const {\n"
        << "        const auto value = read_u8(address);\n"
        << "        return (value & 0x80u) != 0u\n"
        << "            ? 0xFFFFFF00u | static_cast<std::uint32_t>(value)\n"
        << "            : static_cast<std::uint32_t>(value);\n"
        << "    }\n\n"
        << "    [[nodiscard]] std::uint32_t read_s16(\n"
        << "        const std::uint32_t address\n"
        << "    ) const {\n"
        << "        const auto value = read_u16(address);\n"
        << "        return (value & 0x8000u) != 0u\n"
        << "            ? 0xFFFF0000u | static_cast<std::uint32_t>(value)\n"
        << "            : static_cast<std::uint32_t>(value);\n"
        << "    }\n\n"
        << "    void write_u8(\n"
        << "        const std::uint32_t address,\n"
        << "        const std::uint8_t value\n"
        << "    ) {\n"
        << "        check(address, 1u);\n"
        << "        bytes_[static_cast<std::size_t>(address)] = value;\n"
        << "    }\n\n"
        << "    void write_u16(\n"
        << "        const std::uint32_t address,\n"
        << "        const std::uint16_t value\n"
        << "    ) {\n"
        << "        check(address, 2u);\n"
        << "        const auto offset = static_cast<std::size_t>(address);\n"
        << "        bytes_[offset] = static_cast<std::uint8_t>(value & 0xFFu);\n"
        << "        bytes_[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);\n"
        << "    }\n\n"
        << "    void write_u32(\n"
        << "        const std::uint32_t address,\n"
        << "        const std::uint32_t value\n"
        << "    ) {\n"
        << "        check(address, 4u);\n"
        << "        const auto offset = static_cast<std::size_t>(address);\n"
        << "        bytes_[offset] = static_cast<std::uint8_t>(value & 0xFFu);\n"
        << "        bytes_[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);\n"
        << "        bytes_[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);\n"
        << "        bytes_[offset + 3u] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);\n"
        << "    }\n\n"
        << "private:\n"
        << "    void check(\n"
        << "        const std::uint32_t address,\n"
        << "        const std::size_t width\n"
        << "    ) const {\n"
        << "        const auto offset = static_cast<std::size_t>(address);\n"
        << "        if (offset > bytes_.size() || width > bytes_.size() - offset) {\n"
        << "            throw std::out_of_range(\"Speicherzugriff ausserhalb des Runtime-Speichers\");\n"
        << "        }\n"
        << "    }\n\n"
        << "    std::vector<std::uint8_t> bytes_;\n"
        << "};\n\n"
        << "struct CpuState {\n"
        << "    std::array<std::uint32_t, 16> r{};\n"
        << "    std::uint32_t pc = 0;\n"
        << "    std::uint32_t pr = 0;\n"
        << "    bool t = false;\n"
        << "    Memory memory{};\n"
        << "};\n\n"
        << "[[noreturn]] void unresolved_call(\n"
        << "    CpuState&,\n"
        << "    const std::uint32_t\n"
        << ") {\n"
        << "    throw std::runtime_error(\"Nicht aufgeloester Aufruf\");\n"
        << "}\n\n"
        << "[[noreturn]] void unresolved_jump(\n"
        << "    CpuState&,\n"
        << "    const std::uint32_t\n"
        << ") {\n"
        << "    throw std::runtime_error(\"Nicht aufgeloester Sprung\");\n"
        << "}\n\n";

    for (const auto& function : functions) {
        output
            << "static void "
            << function_name(function.entry_address)
            << "(CpuState& cpu);\n";
    }

    output << '\n';

    for (const auto& function : functions) {
        output
            << "static void "
            << function_name(function.entry_address)
            << "(CpuState& cpu) {\n"
            << "    for (;;) {\n"
            << "        switch (cpu.pc) {\n";

        for (const auto& block : function.blocks) {
            emit_block(
                output,
                block,
                known_functions
            );
        }

        output
            << "            default:\n"
            << "                throw std::runtime_error("
            << "\"PC liegt ausserhalb der generierten Funktion\");\n"
            << "        }\n"
            << "    }\n"
            << "}\n\n";
    }

    output
        << "void run(CpuState& cpu) {\n"
        << "    cpu.pc = "
        << hex32(entry_address)
        << ";\n"
        << "    "
        << function_name(entry_address)
        << "(cpu);\n"
        << "}\n\n"
        << "} // namespace katana_generated\n";

    return output.str();
}

}
