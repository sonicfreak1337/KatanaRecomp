#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/verifier.hpp"

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
        (instruction.delay_slot.role == katana::ir::DelaySlotRole::Owner ? 4u : 2u);
}

std::string special_register_read_expression(
    const katana::ir::SpecialRegister special_register
) {
    using Register = katana::ir::SpecialRegister;
    switch (special_register) {
        case Register::Mach: return "cpu.mach";
        case Register::Macl: return "cpu.macl";
        case Register::Pr: return "cpu.pr";
        case Register::Fpul: return "cpu.fpul";
        case Register::Fpscr: return "cpu.read_fpscr()";
        case Register::Sr: return "cpu.read_sr()";
        case Register::Gbr: return "cpu.gbr";
        case Register::Vbr: return "cpu.vbr";
        case Register::Ssr: return "cpu.ssr";
        case Register::Spc: return "cpu.spc";
        case Register::Sgr: return "cpu.sgr";
        case Register::Dbr: return "cpu.dbr";
        case Register::Bank0: return "cpu.r_bank[0]";
        case Register::Bank1: return "cpu.r_bank[1]";
        case Register::Bank2: return "cpu.r_bank[2]";
        case Register::Bank3: return "cpu.r_bank[3]";
        case Register::Bank4: return "cpu.r_bank[4]";
        case Register::Bank5: return "cpu.r_bank[5]";
        case Register::Bank6: return "cpu.r_bank[6]";
        case Register::Bank7: return "cpu.r_bank[7]";
        case Register::None:
            throw std::runtime_error("IR-Transfer ohne Spezialregister.");
    }
    throw std::runtime_error("Unbekanntes Spezialregister.");
}

void emit_special_register_write(
    std::ostringstream& output,
    const katana::ir::SpecialRegister special_register,
    const std::string& value
) {
    using Register = katana::ir::SpecialRegister;
    switch (special_register) {
        case Register::Mach: output << "cpu.mach = " << value << ";\n"; return;
        case Register::Macl: output << "cpu.macl = " << value << ";\n"; return;
        case Register::Pr: output << "cpu.pr = " << value << ";\n"; return;
        case Register::Fpul: output << "cpu.fpul = " << value << ";\n"; return;
        case Register::Fpscr: output << "cpu.write_fpscr(" << value << ");\n"; return;
        case Register::Sr: output << "cpu.write_sr(" << value << ");\n"; return;
        case Register::Gbr: output << "cpu.gbr = " << value << ";\n"; return;
        case Register::Vbr: output << "cpu.vbr = " << value << ";\n"; return;
        case Register::Ssr: output << "cpu.ssr = " << value << ";\n"; return;
        case Register::Spc: output << "cpu.spc = " << value << ";\n"; return;
        case Register::Sgr:
            throw std::runtime_error("SGR kann nicht mit LDC geladen werden.");
        case Register::Dbr: output << "cpu.dbr = " << value << ";\n"; return;
        case Register::Bank0: output << "cpu.r_bank[0] = " << value << ";\n"; return;
        case Register::Bank1: output << "cpu.r_bank[1] = " << value << ";\n"; return;
        case Register::Bank2: output << "cpu.r_bank[2] = " << value << ";\n"; return;
        case Register::Bank3: output << "cpu.r_bank[3] = " << value << ";\n"; return;
        case Register::Bank4: output << "cpu.r_bank[4] = " << value << ";\n"; return;
        case Register::Bank5: output << "cpu.r_bank[5] = " << value << ";\n"; return;
        case Register::Bank6: output << "cpu.r_bank[6] = " << value << ";\n"; return;
        case Register::Bank7: output << "cpu.r_bank[7] = " << value << ";\n"; return;
        case Register::None:
            throw std::runtime_error("IR-Transfer ohne Spezialregister.");
    }
    throw std::runtime_error("Unbekanntes Spezialregister.");
}

void emit_indent(
    std::ostringstream& output,
    const int level
) {
    for (int index = 0; index < level; ++index) {
        output << "    ";
    }
}

bool is_fpu_operation(const katana::ir::Instruction& instruction) {
    using Operation = katana::ir::Operation;
    using Register = katana::ir::SpecialRegister;

    switch (instruction.operation) {
        case Operation::FmovRegister:
        case Operation::FmovLoad:
        case Operation::FmovLoadPostIncrement:
        case Operation::FmovLoadR0Indexed:
        case Operation::FmovStore:
        case Operation::FmovStorePreDecrement:
        case Operation::FmovStoreR0Indexed:
        case Operation::Fldi0:
        case Operation::Fldi1:
        case Operation::Flds:
        case Operation::Fsts:
        case Operation::Fabs:
        case Operation::Fadd:
        case Operation::FcmpEqual:
        case Operation::FcmpGreater:
        case Operation::Fdiv:
        case Operation::FloatFromFpul:
        case Operation::Fmac:
        case Operation::Fmul:
        case Operation::Fneg:
        case Operation::Fsqrt:
        case Operation::Fsub:
        case Operation::Ftrc:
        case Operation::FcnvDoubleToSingle:
        case Operation::FcnvSingleToDouble:
        case Operation::Frchg:
        case Operation::Fschg:
            return true;
        case Operation::StoreSpecialRegister:
        case Operation::StoreSpecialRegisterPreDecrement:
        case Operation::LoadSpecialRegister:
        case Operation::LoadSpecialRegisterPostIncrement:
            return instruction.special_register == Register::Fpul ||
                instruction.special_register == Register::Fpscr;
        default:
            return false;
    }
}

void emit_fpu_disabled_guard(
    std::ostringstream& output,
    const katana::ir::Instruction& instruction,
    const int indent
) {
    if (!is_fpu_operation(instruction)) {
        return;
    }

    emit_indent(output, indent);
    output << "if (cpu.fpu_disabled()) {\n";
    emit_indent(output, indent + 1);
    output << "raise_fpu_disabled(cpu, " << hex32(instruction.source_address);
    if (
        instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot &&
        instruction.delay_slot.counterpart_address.has_value()
    ) {
        output << ", " << hex32(*instruction.delay_slot.counterpart_address);
    }
    output << ");\n";
    emit_indent(output, indent + 1);
    output << "return;\n";
    emit_indent(output, indent);
    output << "}\n";
}

void emit_fpu_mode_guard(
    std::ostringstream& output,
    const katana::ir::Instruction& instruction,
    const int indent
) {
    using Operation = katana::ir::Operation;

    std::string invalid_condition;
    const auto append_condition = [&invalid_condition](const std::string& condition) {
        if (!invalid_condition.empty()) {
            invalid_condition += " || ";
        }
        invalid_condition += condition;
    };

    switch (instruction.operation) {
        case Operation::FmovRegister:
        case Operation::FmovLoad:
        case Operation::FmovLoadPostIncrement:
        case Operation::FmovLoadR0Indexed:
        case Operation::FmovStore:
        case Operation::FmovStorePreDecrement:
        case Operation::FmovStoreR0Indexed:
            append_condition(
                "(cpu.fpu_double_precision() && cpu.fpu_transfer_pair())"
            );
            break;
        case Operation::Fldi0:
        case Operation::Fldi1:
        case Operation::Fmac:
        case Operation::Frchg:
        case Operation::Fschg:
            append_condition("cpu.fpu_double_precision()");
            break;
        case Operation::FcnvDoubleToSingle:
        case Operation::FcnvSingleToDouble:
            append_condition("!cpu.fpu_double_precision()");
            break;
        default:
            break;
    }

    const bool checks_source =
        instruction.operation == Operation::Fadd ||
        instruction.operation == Operation::Fsub ||
        instruction.operation == Operation::Fmul ||
        instruction.operation == Operation::Fdiv ||
        instruction.operation == Operation::FcmpEqual ||
        instruction.operation == Operation::FcmpGreater ||
        instruction.operation == Operation::Ftrc ||
        instruction.operation == Operation::FcnvDoubleToSingle;
    const bool checks_destination =
        instruction.operation == Operation::Fadd ||
        instruction.operation == Operation::Fsub ||
        instruction.operation == Operation::Fmul ||
        instruction.operation == Operation::Fdiv ||
        instruction.operation == Operation::FcmpEqual ||
        instruction.operation == Operation::FcmpGreater ||
        instruction.operation == Operation::Fabs ||
        instruction.operation == Operation::Fneg ||
        instruction.operation == Operation::Fsqrt ||
        instruction.operation == Operation::FloatFromFpul ||
        instruction.operation == Operation::FcnvSingleToDouble;

    if (checks_source && (instruction.source_register & 1u) != 0u) {
        append_condition("cpu.fpu_double_precision()");
    }
    if (checks_destination && (instruction.destination_register & 1u) != 0u) {
        append_condition("cpu.fpu_double_precision()");
    }

    if (invalid_condition.empty()) {
        return;
    }

    emit_indent(output, indent);
    output << "if (" << invalid_condition << ") {\n";
    emit_indent(output, indent + 1);
    output << "raise_illegal_instruction(cpu, " << hex32(instruction.source_address);
    if (
        instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot &&
        instruction.delay_slot.counterpart_address.has_value()
    ) {
        output << ", " << hex32(*instruction.delay_slot.counterpart_address);
    }
    output << ");\n";
    emit_indent(output, indent + 1);
    output << "return;\n";
    emit_indent(output, indent);
    output << "}\n";
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

        case Operation::FmovRegister:
            output << "if (cpu.fpu_transfer_pair()) {\n"
                << "    katana::runtime::write_fpu_pair_bits(cpu, "
                << static_cast<unsigned>(instruction.destination_register) << "u,\n"
                << "        katana::runtime::read_fpu_pair_bits(cpu, "
                << static_cast<unsigned>(instruction.source_register) << "u));\n"
                << "} else {\n"
                << "    cpu.fr[" << static_cast<unsigned>(instruction.destination_register)
                << "] = cpu.fr[" << static_cast<unsigned>(instruction.source_register) << "];\n"
                << "}\n";
            return;
        case Operation::FmovLoad:
        case Operation::FmovLoadPostIncrement:
        case Operation::FmovLoadR0Indexed: {
            const unsigned source = instruction.source_register;
            const unsigned destination = instruction.destination_register;
            output << "{\n"
                << "const std::uint32_t address = ";
            if (instruction.operation == Operation::FmovLoadR0Indexed) {
                output << "cpu.r[0] + ";
            }
            output << "cpu.r[" << source << "];\n"
                << "if (cpu.fpu_transfer_pair()) {\n"
                << "    const std::uint32_t low = cpu.memory.read_u32(address);\n"
                << "    const std::uint32_t high = cpu.memory.read_u32(address + 4u);\n"
                << "    katana::runtime::write_fpu_pair_bits(cpu, " << destination
                << "u, (static_cast<std::uint64_t>(high) << 32u) | low);\n"
                << "} else {\n"
                << "    cpu.fr[" << destination << "] = cpu.memory.read_u32(address);\n"
                << "}\n";
            if (instruction.operation == Operation::FmovLoadPostIncrement) {
                output << "cpu.r[" << source
                    << "] = address + (cpu.fpu_transfer_pair() ? 8u : 4u);\n";
            }
            output << "}\n";
            return;
        }
        case Operation::FmovStore:
        case Operation::FmovStorePreDecrement:
        case Operation::FmovStoreR0Indexed: {
            const unsigned source = instruction.source_register;
            const unsigned destination = instruction.destination_register;
            output << "{\n";
            if (instruction.operation == Operation::FmovStorePreDecrement) {
                output << "const std::uint32_t width = cpu.fpu_transfer_pair() ? 8u : 4u;\n";
            }
            output << "const std::uint32_t address = ";
            if (instruction.operation == Operation::FmovStoreR0Indexed) {
                output << "cpu.r[0] + cpu.r[" << destination << "]";
            } else if (instruction.operation == Operation::FmovStorePreDecrement) {
                output << "cpu.r[" << destination << "] - width";
            } else {
                output << "cpu.r[" << destination << "]";
            }
            output << ";\n"
                << "if (cpu.fpu_transfer_pair()) {\n"
                << "    const std::uint64_t bits = katana::runtime::read_fpu_pair_bits(cpu, "
                << source << "u);\n"
                << "    cpu.memory.write_u32(address, static_cast<std::uint32_t>(bits));\n"
                << "    cpu.memory.write_u32(address + 4u, static_cast<std::uint32_t>(bits >> 32u));\n"
                << "} else {\n"
                << "    cpu.memory.write_u32(address, cpu.fr[" << source << "]);\n"
                << "}\n";
            if (instruction.operation == Operation::FmovStorePreDecrement) {
                output << "cpu.r[" << destination << "] = address;\n";
            }
            output << "}\n";
            return;
        }
        case Operation::Fldi0:
            output << "cpu.fr[" << static_cast<unsigned>(instruction.destination_register)
                << "] = 0x00000000u;\n";
            return;
        case Operation::Fldi1:
            output << "cpu.fr[" << static_cast<unsigned>(instruction.destination_register)
                << "] = 0x3F800000u;\n";
            return;
        case Operation::Flds:
            output << "cpu.fpul = cpu.fr["
                << static_cast<unsigned>(instruction.source_register) << "];\n";
            return;
        case Operation::Fsts:
            output << "cpu.fr[" << static_cast<unsigned>(instruction.destination_register)
                << "] = cpu.fpul;\n";
            return;
        case Operation::Fabs:
            output << "katana::runtime::fpu_absolute(cpu, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::Fadd:
        case Operation::Fsub:
        case Operation::Fmul:
        case Operation::Fdiv: {
            const char* operation = instruction.operation == Operation::Fadd
                ? "Add" : instruction.operation == Operation::Fsub
                ? "Subtract" : instruction.operation == Operation::Fmul
                ? "Multiply" : "Divide";
            output << "katana::runtime::fpu_binary(cpu, "
                << "katana::runtime::FpuBinaryOperation::" << operation << ", "
                << static_cast<unsigned>(instruction.source_register) << "u, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        }
        case Operation::FcmpEqual:
            output << "katana::runtime::fpu_compare_equal(cpu, "
                << static_cast<unsigned>(instruction.source_register) << "u, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::FcmpGreater:
            output << "katana::runtime::fpu_compare_greater(cpu, "
                << static_cast<unsigned>(instruction.source_register) << "u, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::FloatFromFpul:
            output << "katana::runtime::fpu_float_from_fpul(cpu, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::Fmac:
            output << "katana::runtime::fpu_multiply_accumulate(cpu, "
                << static_cast<unsigned>(instruction.source_register) << "u, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::Fneg:
            output << "katana::runtime::fpu_negate(cpu, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::Fsqrt:
            output << "katana::runtime::fpu_square_root(cpu, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::Ftrc:
            output << "katana::runtime::fpu_truncate_to_fpul(cpu, "
                << static_cast<unsigned>(instruction.source_register) << "u);\n";
            return;
        case Operation::FcnvDoubleToSingle:
            output << "katana::runtime::fpu_convert_double_to_single(cpu, "
                << static_cast<unsigned>(instruction.source_register) << "u);\n";
            return;
        case Operation::FcnvSingleToDouble:
            output << "katana::runtime::fpu_convert_single_to_double(cpu, "
                << static_cast<unsigned>(instruction.destination_register) << "u);\n";
            return;
        case Operation::Frchg:
            output << "cpu.toggle_fpu_register_bank();\n";
            return;
        case Operation::Fschg:
            output << "cpu.write_fpscr(cpu.read_fpscr() ^ katana::runtime::fpscr_sz_mask);\n";
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

        case Operation::Constant32:
            output
                << "cpu.r["
                << static_cast<unsigned>(instruction.destination_register)
                << "] = "
                << hex32(static_cast<std::uint32_t>(instruction.immediate))
                << ";\n";
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
        case Operation::ShiftLogicalDynamic:
            output
                << "{\n"
                << "const std::uint32_t count = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "if ((count & 0x80000000u) == 0u) {\n"
                << "    const std::uint32_t amount = count & 0x1Fu;\n"
                << "    cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value << amount;\n"
                << "} else {\n"
                << "    const std::uint32_t amount =\n"
                << "        ((~count) & 0x1Fu) + 1u;\n"
                << "    cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = amount == 32u ? 0u : value >> amount;\n"
                << "}\n"
                << "}\n";
            return;

        case Operation::ShiftArithmeticDynamic:
            output
                << "{\n"
                << "const std::uint32_t count = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "if ((count & 0x80000000u) == 0u) {\n"
                << "    const std::uint32_t amount = count & 0x1Fu;\n"
                << "    cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value << amount;\n"
                << "} else {\n"
                << "    const std::uint32_t amount =\n"
                << "        ((~count) & 0x1Fu) + 1u;\n"
                << "    if (amount == 32u) {\n"
                << "        cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = (value & 0x80000000u) != 0u\n"
                << "            ? 0xFFFFFFFFu\n"
                << "            : 0u;\n"
                << "    } else {\n"
                << "        const std::uint32_t sign_fill =\n"
                << "            (value & 0x80000000u) != 0u\n"
                << "            ? 0xFFFFFFFFu << (32u - amount)\n"
                << "            : 0u;\n"
                << "        cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = (value >> amount) | sign_fill;\n"
                << "    }\n"
                << "}\n"
                << "}\n";
            return;
        case Operation::MultiplyLong:
            output
                << "{\n"
                << "const std::uint64_t product =\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "]) *\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n"
                << "cpu.macl = static_cast<std::uint32_t>(product);\n"
                << "}\n";
            return;

        case Operation::MultiplySignedWord:
            output
                << "{\n"
                << "const std::uint32_t source_raw = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] & 0x0000FFFFu;\n"
                << "const std::uint32_t destination_raw = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] & 0x0000FFFFu;\n"
                << "const std::int32_t source =\n"
                << "    (source_raw & 0x00008000u) != 0u\n"
                << "    ? static_cast<std::int32_t>(source_raw) - 0x00010000\n"
                << "    : static_cast<std::int32_t>(source_raw);\n"
                << "const std::int32_t destination =\n"
                << "    (destination_raw & 0x00008000u) != 0u\n"
                << "    ? static_cast<std::int32_t>(destination_raw) - 0x00010000\n"
                << "    : static_cast<std::int32_t>(destination_raw);\n"
                << "const std::int32_t product = source * destination;\n"
                << "cpu.macl = static_cast<std::uint32_t>(product);\n"
                << "}\n";
            return;

        case Operation::MultiplyUnsignedWord:
            output
                << "cpu.macl =\n"
                << "    (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] & 0x0000FFFFu) *\n"
                << "    (cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] & 0x0000FFFFu);\n";
            return;
        case Operation::DoubleMultiplyUnsignedLong:
            output
                << "{\n"
                << "const std::uint64_t product =\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "]) *\n"
                << "    static_cast<std::uint64_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n"
                << "cpu.mach = static_cast<std::uint32_t>(product >> 32u);\n"
                << "cpu.macl = static_cast<std::uint32_t>(product);\n"
                << "}\n";
            return;

        case Operation::DoubleMultiplySignedLong:
            output
                << "{\n"
                << "const std::uint32_t source_raw = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t destination_raw = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "const std::int64_t source =\n"
                << "    (source_raw & 0x80000000u) != 0u\n"
                << "    ? static_cast<std::int64_t>(source_raw) - 0x100000000ll\n"
                << "    : static_cast<std::int64_t>(source_raw);\n"
                << "const std::int64_t destination =\n"
                << "    (destination_raw & 0x80000000u) != 0u\n"
                << "    ? static_cast<std::int64_t>(destination_raw) - 0x100000000ll\n"
                << "    : static_cast<std::int64_t>(destination_raw);\n"
                << "const std::int64_t signed_product = source * destination;\n"
                << "const std::uint64_t product =\n"
                << "    static_cast<std::uint64_t>(signed_product);\n"
                << "cpu.mach = static_cast<std::uint32_t>(product >> 32u);\n"
                << "cpu.macl = static_cast<std::uint32_t>(product);\n"
                << "}\n";
            return;
        case Operation::MultiplyAccumulateWord:
            output
                << "{\n"
                << "const bool same_register = "
                << (
                    instruction.source_register ==
                    instruction.destination_register
                    ? "true"
                    : "false"
                )
                << ";\n"
                << "const std::uint32_t destination_address = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "const std::uint32_t source_address = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] + (same_register ? 2u : 0u);\n"
                << "const std::uint32_t destination_raw =\n"
                << "    cpu.memory.read_u16(destination_address);\n"
                << "const std::uint32_t source_raw =\n"
                << "    cpu.memory.read_u16(source_address);\n"
                << "const std::int64_t destination =\n"
                << "    (destination_raw & 0x00008000u) != 0u\n"
                << "    ? static_cast<std::int64_t>(destination_raw) - 0x00010000ll\n"
                << "    : static_cast<std::int64_t>(destination_raw);\n"
                << "const std::int64_t source =\n"
                << "    (source_raw & 0x00008000u) != 0u\n"
                << "    ? static_cast<std::int64_t>(source_raw) - 0x00010000ll\n"
                << "    : static_cast<std::int64_t>(source_raw);\n"
                << "const std::int64_t product = source * destination;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] += 2u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] += 2u;\n"
                << "if (cpu.s) {\n"
                << "    const std::int64_t accumulator =\n"
                << "        (cpu.macl & 0x80000000u) != 0u\n"
                << "        ? static_cast<std::int64_t>(cpu.macl) - 0x100000000ll\n"
                << "        : static_cast<std::int64_t>(cpu.macl);\n"
                << "    std::int64_t result = accumulator + product;\n"
                << "    if (result > 0x000000007FFFFFFFll) {\n"
                << "        result = 0x000000007FFFFFFFll;\n"
                << "    } else if (result < -0x0000000080000000ll) {\n"
                << "        result = -0x0000000080000000ll;\n"
                << "    }\n"
                << "    cpu.macl = static_cast<std::uint32_t>(result);\n"
                << "} else {\n"
                << "    const std::uint64_t accumulator =\n"
                << "        (static_cast<std::uint64_t>(cpu.mach) << 32u) |\n"
                << "        static_cast<std::uint64_t>(cpu.macl);\n"
                << "    const std::uint64_t result =\n"
                << "        accumulator + static_cast<std::uint64_t>(product);\n"
                << "    cpu.mach = static_cast<std::uint32_t>(result >> 32u);\n"
                << "    cpu.macl = static_cast<std::uint32_t>(result);\n"
                << "}\n"
                << "}\n";
            return;

        case Operation::MultiplyAccumulateLong:
            output
                << "{\n"
                << "const bool same_register = "
                << (
                    instruction.source_register ==
                    instruction.destination_register
                    ? "true"
                    : "false"
                )
                << ";\n"
                << "const std::uint32_t destination_address = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "const std::uint32_t source_address = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] + (same_register ? 4u : 0u);\n"
                << "const std::uint32_t destination_raw =\n"
                << "    cpu.memory.read_u32(destination_address);\n"
                << "const std::uint32_t source_raw =\n"
                << "    cpu.memory.read_u32(source_address);\n"
                << "const std::int64_t destination =\n"
                << "    (destination_raw & 0x80000000u) != 0u\n"
                << "    ? static_cast<std::int64_t>(destination_raw) - 0x100000000ll\n"
                << "    : static_cast<std::int64_t>(destination_raw);\n"
                << "const std::int64_t source =\n"
                << "    (source_raw & 0x80000000u) != 0u\n"
                << "    ? static_cast<std::int64_t>(source_raw) - 0x100000000ll\n"
                << "    : static_cast<std::int64_t>(source_raw);\n"
                << "const std::int64_t product = source * destination;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] += 4u;\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] += 4u;\n"
                << "if (cpu.s) {\n"
                << "    const std::uint64_t accumulator_raw =\n"
                << "        (static_cast<std::uint64_t>(cpu.mach & 0x0000FFFFu) << 32u) |\n"
                << "        static_cast<std::uint64_t>(cpu.macl);\n"
                << "    const std::int64_t accumulator =\n"
                << "        (accumulator_raw & 0x0000800000000000ull) != 0u\n"
                << "        ? static_cast<std::int64_t>(accumulator_raw) - 0x0001000000000000ll\n"
                << "        : static_cast<std::int64_t>(accumulator_raw);\n"
                << "    std::int64_t result = accumulator + product;\n"
                << "    if (result > 0x00007FFFFFFFFFFFll) {\n"
                << "        result = 0x00007FFFFFFFFFFFll;\n"
                << "    } else if (result < -0x0000800000000000ll) {\n"
                << "        result = -0x0000800000000000ll;\n"
                << "    }\n"
                << "    const std::uint64_t result_bits =\n"
                << "        static_cast<std::uint64_t>(result) & 0x0000FFFFFFFFFFFFull;\n"
                << "    cpu.mach = static_cast<std::uint32_t>(\n"
                << "        (result_bits >> 32u) & 0x0000FFFFu\n"
                << "    );\n"
                << "    cpu.macl = static_cast<std::uint32_t>(result_bits);\n"
                << "} else {\n"
                << "    const std::uint64_t accumulator =\n"
                << "        (static_cast<std::uint64_t>(cpu.mach) << 32u) |\n"
                << "        static_cast<std::uint64_t>(cpu.macl);\n"
                << "    const std::uint64_t result =\n"
                << "        accumulator + static_cast<std::uint64_t>(product);\n"
                << "    cpu.mach = static_cast<std::uint32_t>(result >> 32u);\n"
                << "    cpu.macl = static_cast<std::uint32_t>(result);\n"
                << "}\n"
                << "}\n";
            return;
        case Operation::DivideInitializeUnsigned:
            output
                << "cpu.q = false;\n"
                << "cpu.m = false;\n"
                << "cpu.t = false;\n";
            return;

        case Operation::DivideInitializeSigned:
            output
                << "cpu.q = (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] & 0x80000000u) != 0u;\n"
                << "cpu.m = (cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] & 0x80000000u) != 0u;\n"
                << "cpu.t = cpu.m != cpu.q;\n";
            return;

        case Operation::DivideStep:
            output
                << "{\n"
                << "const bool old_q = cpu.q;\n"
                << "cpu.q = (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] & 0x80000000u) != 0u;\n"
                << "const std::uint32_t divisor = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = (cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] << 1u) | (cpu.t ? 1u : 0u);\n"
                << "const std::uint32_t shifted = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "];\n"
                << "if (!old_q) {\n"
                << "    if (!cpu.m) {\n"
                << "        cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = shifted - divisor;\n"
                << "        const bool borrow = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] > shifted;\n"
                << "        cpu.q = cpu.q != borrow;\n"
                << "    } else {\n"
                << "        cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = shifted + divisor;\n"
                << "        const bool carry = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] < shifted;\n"
                << "        cpu.q = (!cpu.q) != carry;\n"
                << "    }\n"
                << "} else {\n"
                << "    if (!cpu.m) {\n"
                << "        cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = shifted + divisor;\n"
                << "        const bool carry = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] < shifted;\n"
                << "        cpu.q = cpu.q != carry;\n"
                << "    } else {\n"
                << "        cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = shifted - divisor;\n"
                << "        const bool borrow = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] > shifted;\n"
                << "        cpu.q = (!cpu.q) != borrow;\n"
                << "    }\n"
                << "}\n"
                << "cpu.t = cpu.q == cpu.m;\n"
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
        case Operation::ClearS:
            output << "cpu.s = false;\n";
            return;

        case Operation::SetS:
            output << "cpu.s = true;\n";
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
            if (instruction.forwarded_value_register) {
                output
                    << "{\n"
                    << "const std::uint32_t forwarded_value = cpu.r["
                    << static_cast<unsigned>(*instruction.forwarded_value_register)
                    << "];\n"
                    << "static_cast<void>(cpu.memory.read_u32(cpu.r["
                    << static_cast<unsigned>(instruction.source_register)
                    << "]));\n"
                    << "cpu.r["
                    << static_cast<unsigned>(instruction.destination_register)
                    << "] = forwarded_value;\n"
                    << "}\n";
            } else {
                output
                    << "cpu.r["
                    << static_cast<unsigned>(instruction.destination_register)
                    << "] = cpu.memory.read_u32(cpu.r["
                    << static_cast<unsigned>(instruction.source_register)
                    << "]);\n";
            }
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

        case Operation::StoreByteDisplacement:
            output
                << "cpu.memory.write_u8(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u, static_cast<std::uint8_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]));\n";
            return;

        case Operation::StoreWordDisplacement:
            output
                << "cpu.memory.write_u16(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u, static_cast<std::uint16_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]));\n";
            return;

        case Operation::StoreLongDisplacement:
            output
                << "cpu.memory.write_u32(cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u, cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::LoadByteSignedDisplacement:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_s8(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u);\n";
            return;

        case Operation::LoadWordSignedDisplacement:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_s16(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u);\n";
            return;

        case Operation::LoadLongDisplacement:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_u32(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u);\n";
            return;

        case Operation::StoreByteR0Indexed:
            output
                << "cpu.memory.write_u8(cpu.r[0] + cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "], static_cast<std::uint8_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]));\n";
            return;

        case Operation::StoreWordR0Indexed:
            output
                << "cpu.memory.write_u16(cpu.r[0] + cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "], static_cast<std::uint16_t>(cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]));\n";
            return;

        case Operation::StoreLongR0Indexed:
            output
                << "cpu.memory.write_u32(cpu.r[0] + cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "], cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::LoadByteSignedR0Indexed:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_s8(cpu.r[0] + cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::LoadWordSignedR0Indexed:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_s16(cpu.r[0] + cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::LoadLongR0Indexed:
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_u32(cpu.r[0] + cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]);\n";
            return;

        case Operation::StoreByteGbrDisplacement:
            output
                << "cpu.memory.write_u8(cpu.gbr + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u, static_cast<std::uint8_t>(cpu.r[0]));\n";
            return;

        case Operation::StoreWordGbrDisplacement:
            output
                << "cpu.memory.write_u16(cpu.gbr + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u, static_cast<std::uint16_t>(cpu.r[0]));\n";
            return;

        case Operation::StoreLongGbrDisplacement:
            output
                << "cpu.memory.write_u32(cpu.gbr + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u, cpu.r[0]);\n";
            return;

        case Operation::LoadByteSignedGbrDisplacement:
            output
                << "cpu.r[0] = cpu.memory.read_s8(cpu.gbr + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u);\n";
            return;

        case Operation::LoadWordSignedGbrDisplacement:
            output
                << "cpu.r[0] = cpu.memory.read_s16(cpu.gbr + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u);\n";
            return;

        case Operation::LoadLongGbrDisplacement:
            output
                << "cpu.r[0] = cpu.memory.read_u32(cpu.gbr + "
                << static_cast<std::uint32_t>(
                    instruction.displacement
                )
                << "u);\n";
            return;

        case Operation::LoadWordSignedPcRelative:
            if (!instruction.effective_address.has_value()) {
                throw std::runtime_error(
                    "PC-relativem Word-Load fehlt die effektive Adresse."
                );
            }
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_s16("
                << hex32(*instruction.effective_address)
                << ");\n";
            return;

        case Operation::LoadLongPcRelative:
            if (!instruction.effective_address.has_value()) {
                throw std::runtime_error(
                    "PC-relativem Long-Load fehlt die effektive Adresse."
                );
            }
            output
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = cpu.memory.read_u32("
                << hex32(*instruction.effective_address)
                << ");\n";
            return;

        case Operation::MoveAddressPcRelative:
            if (!instruction.effective_address.has_value()) {
                throw std::runtime_error(
                    "MOVA fehlt die effektive Adresse."
                );
            }
            output
                << "cpu.r[0] = "
                << hex32(*instruction.effective_address)
                << ";\n";
            return;

        case Operation::StoreSpecialRegister:
            output
                << "cpu.r["
                << static_cast<unsigned>(instruction.destination_register)
                << "] = "
                << special_register_read_expression(instruction.special_register)
                << ";\n";
            return;

        case Operation::StoreSpecialRegisterPreDecrement:
            output
                << "{\n"
                << "const std::uint32_t value = "
                << special_register_read_expression(instruction.special_register)
                << ";\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(instruction.destination_register)
                << "] - 4u;\n"
                << "cpu.memory.write_u32(address, value);\n"
                << "cpu.r["
                << static_cast<unsigned>(instruction.destination_register)
                << "] = address;\n"
                << "}\n";
            return;

        case Operation::LoadSpecialRegister: {
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(instruction.source_register)
                << "];\n";
            emit_special_register_write(
                output,
                instruction.special_register,
                "value"
            );
            output << "}\n";
            return;
        }

        case Operation::LoadSpecialRegisterPostIncrement: {
            output
                << "{\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(instruction.source_register)
                << "];\n"
                << "const std::uint32_t value = cpu.memory.read_u32(address);\n"
                << "cpu.r["
                << static_cast<unsigned>(instruction.source_register)
                << "] = address + 4u;\n";
            emit_special_register_write(
                output,
                instruction.special_register,
                "value"
            );
            output << "}\n";
            return;
        }


        case Operation::StoreBytePreDecrement:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] - 1u;\n"
                << "cpu.memory.write_u8(\n"
                << "    address,\n"
                << "    static_cast<std::uint8_t>(value)\n"
                << ");\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = address;\n"
                << "}\n";
            return;

        case Operation::StoreWordPreDecrement:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] - 2u;\n"
                << "cpu.memory.write_u16(\n"
                << "    address,\n"
                << "    static_cast<std::uint16_t>(value)\n"
                << ");\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = address;\n"
                << "}\n";
            return;

        case Operation::StoreLongPreDecrement:
            output
                << "{\n"
                << "const std::uint32_t value = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] - 4u;\n"
                << "cpu.memory.write_u32(address, value);\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = address;\n"
                << "}\n";
            return;

        case Operation::LoadByteSignedPostIncrement:
            output
                << "{\n"
                << "const bool same_register = "
                << (
                    instruction.source_register ==
                    instruction.destination_register
                    ? "true"
                    : "false"
                )
                << ";\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t value =\n"
                << "    cpu.memory.read_s8(address);\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value;\n"
                << "if (!same_register) {\n"
                << "    cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] = address + 1u;\n"
                << "}\n"
                << "}\n";
            return;

        case Operation::LoadWordSignedPostIncrement:
            output
                << "{\n"
                << "const bool same_register = "
                << (
                    instruction.source_register ==
                    instruction.destination_register
                    ? "true"
                    : "false"
                )
                << ";\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t value =\n"
                << "    cpu.memory.read_s16(address);\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value;\n"
                << "if (!same_register) {\n"
                << "    cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] = address + 2u;\n"
                << "}\n"
                << "}\n";
            return;

        case Operation::LoadLongPostIncrement:
            output
                << "{\n"
                << "const bool same_register = "
                << (
                    instruction.source_register ==
                    instruction.destination_register
                    ? "true"
                    : "false"
                )
                << ";\n"
                << "const std::uint32_t address = cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "];\n"
                << "const std::uint32_t value =\n"
                << "    cpu.memory.read_u32(address);\n"
                << "cpu.r["
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "] = value;\n"
                << "if (!same_register) {\n"
                << "    cpu.r["
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "] = address + 4u;\n"
                << "}\n"
                << "}\n";
            return;
        case Operation::Unknown:
            output << "raise_illegal_instruction(cpu, "
                << hex32(instruction.source_address);
            if (
                instruction.delay_slot.role ==
                    katana::ir::DelaySlotRole::Slot &&
                instruction.delay_slot.counterpart_address.has_value()
            ) {
                output << ", "
                    << hex32(*instruction.delay_slot.counterpart_address);
            }
            output << ");\n";
            emit_indent(output, indent);
            output << "if (cpu.trap_pending) {\n";
            emit_indent(output, indent + 1);
            output << "return;\n";
            emit_indent(output, indent);
            output << "}\n";
            return;

        case Operation::Branch:
        case Operation::Call:
        case Operation::BranchIfTrue:
        case Operation::BranchIfFalse:
        case Operation::JumpRegister:
        case Operation::CallRegister:
        case Operation::Return:
        case Operation::TrapAlways:
        case Operation::ReturnFromException:
        case Operation::Sleep:
            output
                << "throw std::runtime_error("
                << "\"Kontrollfluss im Delay Slot wird nicht unterstuetzt\");\n";
            return;
    }
}

void emit_guarded_simple_instruction(
    std::ostringstream& output,
    const katana::ir::Instruction& instruction,
    const int indent
) {
    emit_fpu_disabled_guard(output, instruction, indent);
    emit_fpu_mode_guard(output, instruction, indent);

    if (
        instruction.memory_effects.access ==
        katana::ir::MemoryAccessKind::None
    ) {
        emit_simple_instruction(output, instruction, indent);
        return;
    }

    emit_indent(output, indent);
    output << "try {\n";
    emit_simple_instruction(output, instruction, indent + 1);
    emit_indent(output, indent);
    output
        << "} catch (const katana::runtime::MemoryAccessError& error) {\n";
    emit_indent(output, indent + 1);
    output << "enter_memory_exception(cpu, error, "
        << hex32(instruction.source_address);
    if (
        instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot &&
        instruction.delay_slot.counterpart_address.has_value()
    ) {
        output << ", "
            << hex32(*instruction.delay_slot.counterpart_address);
    }
    output << ");\n";
    emit_indent(output, indent + 1);
    output << "return;\n";
    emit_indent(output, indent);
    output << "}\n";
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
        emit_indent(output, indent);
        output << "if (cpu.trap_pending) {\n";
        emit_indent(output, indent + 1);
        output << "return;\n";
        emit_indent(output, indent);
        output << "}\n";
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
    const std::unordered_set<std::uint32_t>& current_blocks,
    const int indent
) {
    using Operation = katana::ir::Operation;

    const auto& instruction =
        block.instructions[control_index];

    const katana::ir::Instruction* delay_slot = nullptr;

    if (
        instruction.delay_slot.role == katana::ir::DelaySlotRole::Owner &&
        control_index + 1u < block.instructions.size() &&
        block.instructions[control_index + 1u].delay_slot.role ==
            katana::ir::DelaySlotRole::Slot &&
        instruction.delay_slot.counterpart_address ==
            block.instructions[control_index + 1u].source_address &&
        block.instructions[control_index + 1u].delay_slot.counterpart_address ==
            instruction.source_address
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
                emit_guarded_simple_instruction(
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
                emit_guarded_simple_instruction(
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

                emit_guarded_simple_instruction(
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
                emit_guarded_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );
            }

            if (instruction.resolved_targets.empty()) {
                emit_indent(output, indent);
                output << "unresolved_jump(cpu, jump_target);\n";

                emit_indent(output, indent);
                output << "return;\n";
                return;
            }

            emit_indent(output, indent);
            output << "switch (jump_target) {\n";
            for (const auto target : instruction.resolved_targets) {
                emit_indent(output, indent + 1);
                output << "case " << hex32(target) << ":\n";
                if (current_blocks.contains(target)) {
                    emit_indent(output, indent + 2);
                    output << "cpu.pc = " << hex32(target) << ";\n";
                    emit_indent(output, indent + 2);
                    output << "continue;\n";
                } else if (known_functions.contains(target)) {
                    emit_indent(output, indent + 2);
                    output << "cpu.pc = " << hex32(target) << ";\n";
                    emit_indent(output, indent + 2);
                    output << function_name(target) << "(cpu);\n";
                    emit_indent(output, indent + 2);
                    output << "return;\n";
                } else {
                    emit_indent(output, indent + 2);
                    output << "unresolved_jump(cpu, jump_target);\n";
                }
            }
            emit_indent(output, indent + 1);
            output << "default:\n";
            emit_indent(output, indent + 2);
            output << "unresolved_jump(cpu, jump_target);\n";
            emit_indent(output, indent);
            output << "}\n";
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
                emit_guarded_simple_instruction(
                    output,
                    *delay_slot,
                    indent
                );
            }

            if (instruction.resolved_targets.empty()) {
                emit_indent(output, indent);
                output << "unresolved_call(cpu, call_target);\n";
            } else {
                emit_indent(output, indent);
                output << "switch (call_target) {\n";
                for (const auto target : instruction.resolved_targets) {
                    emit_indent(output, indent + 1);
                    output << "case " << hex32(target) << ":\n";
                    if (known_functions.contains(target)) {
                        emit_direct_call(
                            output,
                            target,
                            known_functions,
                            indent + 2
                        );
                        emit_indent(output, indent + 2);
                        output << "break;\n";
                    } else {
                        emit_indent(output, indent + 2);
                        output << "unresolved_call(cpu, call_target);\n";
                    }
                }
                emit_indent(output, indent + 1);
                output << "default:\n";
                emit_indent(output, indent + 2);
                output << "unresolved_call(cpu, call_target);\n";
                emit_indent(output, indent);
                output << "}\n";
            }

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
                emit_guarded_simple_instruction(
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

        case Operation::TrapAlways:
            emit_indent(output, indent);
            output
                << "raise_trapa(cpu, "
                << static_cast<unsigned>(instruction.immediate)
                << "u, "
                << hex32(instruction.source_address)
                << ");\n";
            emit_indent(output, indent);
            output << "return;\n";
            return;

        case Operation::ReturnFromException:
            emit_indent(output, indent);
            output << "return_from_exception(cpu);\n";
            if (delay_slot != nullptr) {
                emit_guarded_simple_instruction(output, *delay_slot, indent);
            }
            emit_indent(output, indent);
            output << "return;\n";
            return;

        case Operation::Sleep:
            emit_indent(output, indent);
            output << "cpu.sleeping = true;\n";
            emit_indent(output, indent);
            output << "cpu.pc = "
                << hex32(instruction.source_address + 2u)
                << ";\n";
            emit_indent(output, indent);
            output << "return;\n";
            return;

        case Operation::Unknown:
        case Operation::Nop:
        case Operation::MovImmediate:
        case Operation::Constant32:
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
        case Operation::ShiftArithmeticDynamic:
        case Operation::ShiftLogicalDynamic:
        case Operation::MultiplyLong:
        case Operation::MultiplySignedWord:
        case Operation::MultiplyUnsignedWord:
        case Operation::DoubleMultiplySignedLong:
        case Operation::DoubleMultiplyUnsignedLong:
        case Operation::MultiplyAccumulateWord:
        case Operation::MultiplyAccumulateLong:
        case Operation::DivideInitializeUnsigned:
        case Operation::DivideInitializeSigned:
        case Operation::DivideStep:
        case Operation::AndRegister:
        case Operation::OrRegister:
        case Operation::XorRegister:
        case Operation::AndImmediate:
        case Operation::OrImmediate:
        case Operation::XorImmediate:
        case Operation::ClearS:
        case Operation::SetS:
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
        case Operation::StoreBytePreDecrement:
        case Operation::StoreWordPreDecrement:
        case Operation::StoreLongPreDecrement:
        case Operation::LoadByteSignedPostIncrement:
        case Operation::LoadWordSignedPostIncrement:
        case Operation::LoadLongPostIncrement:
        case Operation::StoreByteDisplacement:
        case Operation::StoreWordDisplacement:
        case Operation::StoreLongDisplacement:
        case Operation::LoadByteSignedDisplacement:
        case Operation::LoadWordSignedDisplacement:
        case Operation::LoadLongDisplacement:
        case Operation::StoreByteR0Indexed:
        case Operation::StoreWordR0Indexed:
        case Operation::StoreLongR0Indexed:
        case Operation::LoadByteSignedR0Indexed:
        case Operation::LoadWordSignedR0Indexed:
        case Operation::LoadLongR0Indexed:
        case Operation::StoreByteGbrDisplacement:
        case Operation::StoreWordGbrDisplacement:
        case Operation::StoreLongGbrDisplacement:
        case Operation::LoadByteSignedGbrDisplacement:
        case Operation::LoadWordSignedGbrDisplacement:
        case Operation::LoadLongGbrDisplacement:
        case Operation::LoadWordSignedPcRelative:
        case Operation::LoadLongPcRelative:
        case Operation::MoveAddressPcRelative:
        case Operation::StoreSpecialRegister:
        case Operation::StoreSpecialRegisterPreDecrement:
        case Operation::LoadSpecialRegister:
        case Operation::LoadSpecialRegisterPostIncrement:
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
        case Operation::TrapAlways:
        case Operation::ReturnFromException:
        case Operation::Sleep:
            return true;

        case Operation::Unknown:
        case Operation::Nop:
        case Operation::MovImmediate:
        case Operation::Constant32:
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
        case Operation::ShiftArithmeticDynamic:
        case Operation::ShiftLogicalDynamic:
        case Operation::MultiplyLong:
        case Operation::MultiplySignedWord:
        case Operation::MultiplyUnsignedWord:
        case Operation::DoubleMultiplySignedLong:
        case Operation::DoubleMultiplyUnsignedLong:
        case Operation::MultiplyAccumulateWord:
        case Operation::MultiplyAccumulateLong:
        case Operation::DivideInitializeUnsigned:
        case Operation::DivideInitializeSigned:
        case Operation::DivideStep:
        case Operation::AndRegister:
        case Operation::OrRegister:
        case Operation::XorRegister:
        case Operation::AndImmediate:
        case Operation::OrImmediate:
        case Operation::XorImmediate:
        case Operation::ClearS:
        case Operation::SetS:
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
        case Operation::StoreBytePreDecrement:
        case Operation::StoreWordPreDecrement:
        case Operation::StoreLongPreDecrement:
        case Operation::LoadByteSignedPostIncrement:
        case Operation::LoadWordSignedPostIncrement:
        case Operation::LoadLongPostIncrement:
        case Operation::StoreByteDisplacement:
        case Operation::StoreWordDisplacement:
        case Operation::StoreLongDisplacement:
        case Operation::LoadByteSignedDisplacement:
        case Operation::LoadWordSignedDisplacement:
        case Operation::LoadLongDisplacement:
        case Operation::StoreByteR0Indexed:
        case Operation::StoreWordR0Indexed:
        case Operation::StoreLongR0Indexed:
        case Operation::LoadByteSignedR0Indexed:
        case Operation::LoadWordSignedR0Indexed:
        case Operation::LoadLongR0Indexed:
        case Operation::StoreByteGbrDisplacement:
        case Operation::StoreWordGbrDisplacement:
        case Operation::StoreLongGbrDisplacement:
        case Operation::LoadByteSignedGbrDisplacement:
        case Operation::LoadWordSignedGbrDisplacement:
        case Operation::LoadLongGbrDisplacement:
        case Operation::LoadWordSignedPcRelative:
        case Operation::LoadLongPcRelative:
        case Operation::MoveAddressPcRelative:
        case Operation::StoreSpecialRegister:
        case Operation::StoreSpecialRegisterPreDecrement:
        case Operation::LoadSpecialRegister:
        case Operation::LoadSpecialRegisterPostIncrement:
            return false;
    }

    return false;
}

void emit_block(
    std::ostringstream& output,
    const katana::ir::BasicBlock& block,
    const std::unordered_set<std::uint32_t>& known_functions,
    const std::unordered_set<std::uint32_t>& current_blocks
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

        if (instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot) {
            continue;
        }

        if (is_control_flow(instruction.operation)) {
            control_index = index;
            break;
        }

        emit_guarded_simple_instruction(
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
            current_blocks,
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

    katana::ir::require_valid_program(functions);

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
        << "#include \"katana/runtime/exception.hpp\"\n"
        << "#include \"katana/runtime/fpu.hpp\"\n"
        << "#include \"katana/runtime/runtime.hpp\"\n"
        << "#include <cstdint>\n"
        << "#include <stdexcept>\n\n"
        << "namespace katana_generated {\n\n"
        << "inline constexpr std::uint32_t required_runtime_abi = 7u;\n"
        << "static_assert(\n"
        << "    katana::runtime::abi_version == required_runtime_abi,\n"
        << "    \"Inkompatible Katana-Runtime-ABI\"\n"
        << ");\n\n"
        << "using CpuState = katana::runtime::CpuState;\n"
        << "using Memory = katana::runtime::Memory;\n"
        << "using katana::runtime::enter_memory_exception;\n"
        << "using katana::runtime::raise_fpu_disabled;\n"
        << "using katana::runtime::raise_illegal_instruction;\n"
        << "using katana::runtime::raise_trapa;\n"
        << "using katana::runtime::return_from_exception;\n"
        << "using katana::runtime::unresolved_call;\n"
        << "using katana::runtime::unresolved_jump;\n\n";

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

        std::unordered_set<std::uint32_t> current_blocks;
        current_blocks.reserve(function.blocks.size());
        for (const auto& block : function.blocks) {
            current_blocks.insert(block.start_address);
        }

        for (const auto& block : function.blocks) {
            emit_block(
                output,
                block,
                known_functions,
                current_blocks
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
