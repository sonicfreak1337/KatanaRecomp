#include "katana/ir/serialize.hpp"

#include "katana/ir/verifier.hpp"
#include "katana/io/json_report.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace katana::ir {
namespace {

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
        << std::setfill('0') << value;
    return output.str();
}

std::string hex16(const std::uint16_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(4)
        << std::setfill('0') << value;
    return output.str();
}

const char* boolean_name(const bool value) noexcept {
    return value ? "true" : "false";
}

std::string_view special_register_name(const SpecialRegister value) noexcept {
    switch (value) {
        case SpecialRegister::None: return "none";
        case SpecialRegister::Mach: return "mach";
        case SpecialRegister::Macl: return "macl";
        case SpecialRegister::Pr: return "pr";
        case SpecialRegister::Fpul: return "fpul";
        case SpecialRegister::Fpscr: return "fpscr";
        case SpecialRegister::Sr: return "sr";
        case SpecialRegister::Gbr: return "gbr";
        case SpecialRegister::Vbr: return "vbr";
        case SpecialRegister::Ssr: return "ssr";
        case SpecialRegister::Spc: return "spc";
        case SpecialRegister::Sgr: return "sgr";
        case SpecialRegister::Dbr: return "dbr";
        case SpecialRegister::Bank0: return "r0_bank";
        case SpecialRegister::Bank1: return "r1_bank";
        case SpecialRegister::Bank2: return "r2_bank";
        case SpecialRegister::Bank3: return "r3_bank";
        case SpecialRegister::Bank4: return "r4_bank";
        case SpecialRegister::Bank5: return "r5_bank";
        case SpecialRegister::Bank6: return "r6_bank";
        case SpecialRegister::Bank7: return "r7_bank";
    }
    return "unknown";
}

std::string_view memory_access_name(const MemoryAccessKind value) noexcept {
    switch (value) {
        case MemoryAccessKind::None: return "none";
        case MemoryAccessKind::Read: return "read";
        case MemoryAccessKind::Write: return "write";
    }
    return "unknown";
}

std::string_view address_update_name(const AddressUpdateKind value) noexcept {
    switch (value) {
        case AddressUpdateKind::None: return "none";
        case AddressUpdateKind::PreDecrement: return "pre_decrement";
        case AddressUpdateKind::PostIncrement: return "post_increment";
    }
    return "unknown";
}

std::string_view memory_region_name(const MemoryRegionKind value) noexcept {
    switch (value) {
        case MemoryRegionKind::Unknown: return "unknown";
        case MemoryRegionKind::NormalRam: return "normal_ram";
        case MemoryRegionKind::Volatile: return "volatile";
    }
    return "unknown";
}

std::string_view delay_role_name(const DelaySlotRole value) noexcept {
    switch (value) {
        case DelaySlotRole::None: return "none";
        case DelaySlotRole::Owner: return "owner";
        case DelaySlotRole::Slot: return "slot";
    }
    return "unknown";
}

std::vector<std::string_view> status_names(const StatusRegisterBit mask) {
    std::vector<std::string_view> names;
    const auto raw = static_cast<std::uint8_t>(mask);
    const auto has_raw = [raw](const StatusRegisterBit bit) {
        return (raw & static_cast<std::uint8_t>(bit)) != 0u;
    };
    if (has_raw(StatusRegisterBit::T)) names.push_back("t");
    if (has_raw(StatusRegisterBit::S)) names.push_back("s");
    if (has_raw(StatusRegisterBit::Q)) names.push_back("q");
    if (has_raw(StatusRegisterBit::M)) names.push_back("m");
    if (has_raw(StatusRegisterBit::Full)) names.push_back("full");
    return names;
}

std::vector<std::string_view> accumulator_names(const AccumulatorRegister mask) {
    std::vector<std::string_view> names;
    if (contains_accumulator_register(mask, AccumulatorRegister::Mach)) {
        names.push_back("mach");
    }
    if (contains_accumulator_register(mask, AccumulatorRegister::Macl)) {
        names.push_back("macl");
    }
    return names;
}

template <typename Value>
std::vector<Value> sorted_values(const std::vector<Value>& values) {
    auto result = values;
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<const Function*> sorted_functions(
    const std::span<const Function> functions
) {
    std::vector<const Function*> result;
    result.reserve(functions.size());
    require_valid_program(functions);
    for (const auto& function : functions) result.push_back(&function);
    std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return left->entry_address < right->entry_address;
    });
    for (std::size_t index = 1u; index < result.size(); ++index) {
        if (result[index - 1u]->entry_address == result[index]->entry_address) {
            throw std::invalid_argument(
                "IR-Programm besitzt doppelte Funktionseinstiege."
            );
        }
    }
    return result;
}

std::vector<const BasicBlock*> sorted_blocks(const Function& function) {
    std::vector<const BasicBlock*> result;
    result.reserve(function.blocks.size());
    for (const auto& block : function.blocks) result.push_back(&block);
    std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return left->start_address < right->start_address;
    });
    return result;
}

std::vector<const Instruction*> sorted_instructions(const BasicBlock& block) {
    std::vector<const Instruction*> result;
    result.reserve(block.instructions.size());
    for (const auto& instruction : block.instructions) result.push_back(&instruction);
    std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return left->source_address < right->source_address;
    });
    return result;
}

void emit_text_names(
    std::ostringstream& output,
    const std::vector<std::string_view>& names
) {
    output << '[';
    for (std::size_t index = 0u; index < names.size(); ++index) {
        if (index != 0u) output << ',';
        output << names[index];
    }
    output << ']';
}

void emit_json_names(
    std::ostringstream& output,
    const std::vector<std::string_view>& names
) {
    output << '[';
    for (std::size_t index = 0u; index < names.size(); ++index) {
        if (index != 0u) output << ',';
        output << '"' << names[index] << '"';
    }
    output << ']';
}

void emit_text_addresses(
    std::ostringstream& output,
    const std::vector<std::uint32_t>& addresses
) {
    output << '[';
    for (std::size_t index = 0u; index < addresses.size(); ++index) {
        if (index != 0u) output << ',';
        output << hex32(addresses[index]);
    }
    output << ']';
}

void emit_json_addresses(
    std::ostringstream& output,
    const std::vector<std::uint32_t>& addresses
) {
    output << '[';
    for (std::size_t index = 0u; index < addresses.size(); ++index) {
        if (index != 0u) output << ',';
        output << '"' << hex32(addresses[index]) << '"';
    }
    output << ']';
}

void emit_text_instruction(std::ostringstream& output, const Instruction& value) {
    output << "    instruction " << hex32(value.source_address)
        << " opcode=" << hex16(value.original_opcode)
        << " original_operation=" << operation_name(value.original_operation)
        << " operation=" << operation_name(value.operation) << '\n'
        << "      widths result=" << operand_width_name(value.widths.result)
        << " input=" << operand_width_name(value.widths.input)
        << " immediate=" << operand_width_name(value.widths.immediate)
        << " displacement=" << operand_width_name(value.widths.displacement)
        << " memory=" << operand_width_name(value.widths.memory)
        << " address=" << operand_width_name(value.widths.address) << '\n'
        << "      operands destination=r" << static_cast<unsigned>(value.destination_register)
        << " source=r" << static_cast<unsigned>(value.source_register)
        << " branch=r" << static_cast<unsigned>(value.branch_register)
        << " immediate=" << value.immediate
        << " displacement=" << value.displacement
        << " special=" << special_register_name(value.special_register)
        << " forwarded=" << (value.forwarded_value_register
            ? "r" + std::to_string(*value.forwarded_value_register) : "null")
        << " effective=" << (value.effective_address ? hex32(*value.effective_address) : "null")
        << " target=" << (value.target_address ? hex32(*value.target_address) : "null")
        << " resolved_targets=";
    emit_text_addresses(output, sorted_values(value.resolved_targets));
    output << '\n'
        << "      status reads=";
    emit_text_names(output, status_names(value.status_effects.reads));
    output << " writes=";
    emit_text_names(output, status_names(value.status_effects.writes));
    output << '\n' << "      memory access=" << memory_access_name(value.memory_effects.access)
        << " width=" << operand_width_name(value.memory_effects.width)
        << " count=" << static_cast<unsigned>(value.memory_effects.access_count)
        << " update=" << address_update_name(value.memory_effects.address_update)
        << " updated_registers="
        << static_cast<unsigned>(value.memory_effects.updated_register_count)
        << " region=" << memory_region_name(value.memory_effects.region) << '\n'
        << "      accumulator reads_s0=";
    emit_text_names(output, accumulator_names(value.accumulator_effects.reads_if_s_clear));
    output << " reads_s1=";
    emit_text_names(output, accumulator_names(value.accumulator_effects.reads_if_s_set));
    output << " writes_s0=";
    emit_text_names(output, accumulator_names(value.accumulator_effects.writes_if_s_clear));
    output << " writes_s1=";
    emit_text_names(output, accumulator_names(value.accumulator_effects.writes_if_s_set));
    output << '\n' << "      delay role=" << delay_role_name(value.delay_slot.role)
        << " counterpart="
        << (value.delay_slot.counterpart_address
            ? hex32(*value.delay_slot.counterpart_address) : "null")
        << " privileged=" << boolean_name(value.is_privileged) << '\n';
}

void emit_json_instruction(std::ostringstream& output, const Instruction& value) {
    output << '{'
        << "\"address\":\"" << hex32(value.source_address) << "\","
        << "\"opcode\":\"" << hex16(value.original_opcode) << "\","
        << "\"original_operation\":\"" << operation_name(value.original_operation) << "\","
        << "\"operation\":\"" << operation_name(value.operation) << "\","
        << "\"widths\":{"
        << "\"result\":\"" << operand_width_name(value.widths.result) << "\","
        << "\"input\":\"" << operand_width_name(value.widths.input) << "\","
        << "\"immediate\":\"" << operand_width_name(value.widths.immediate) << "\","
        << "\"displacement\":\"" << operand_width_name(value.widths.displacement) << "\","
        << "\"memory\":\"" << operand_width_name(value.widths.memory) << "\","
        << "\"address\":\"" << operand_width_name(value.widths.address) << "\"},"
        << "\"operands\":{"
        << "\"destination_register\":" << static_cast<unsigned>(value.destination_register) << ','
        << "\"source_register\":" << static_cast<unsigned>(value.source_register) << ','
        << "\"branch_register\":" << static_cast<unsigned>(value.branch_register) << ','
        << "\"immediate\":" << value.immediate << ','
        << "\"displacement\":" << value.displacement << ','
        << "\"special_register\":\"" << special_register_name(value.special_register) << "\","
        << "\"forwarded_value_register\":";
    if (value.forwarded_value_register) {
        output << static_cast<unsigned>(*value.forwarded_value_register);
    } else {
        output << "null";
    }
    output << ','
        << "\"effective_address\":";
    if (value.effective_address) output << '"' << hex32(*value.effective_address) << '"';
    else output << "null";
    output << ",\"target_address\":";
    if (value.target_address) output << '"' << hex32(*value.target_address) << '"';
    else output << "null";
    output << ",\"resolved_targets\":";
    emit_json_addresses(output, sorted_values(value.resolved_targets));
    output << "},\"status\":{\"reads\":";
    emit_json_names(output, status_names(value.status_effects.reads));
    output << ",\"writes\":";
    emit_json_names(output, status_names(value.status_effects.writes));
    output << "},\"memory_effects\":{"
        << "\"access\":\"" << memory_access_name(value.memory_effects.access) << "\","
        << "\"width\":\"" << operand_width_name(value.memory_effects.width) << "\","
        << "\"access_count\":" << static_cast<unsigned>(value.memory_effects.access_count) << ','
        << "\"address_update\":\"" << address_update_name(value.memory_effects.address_update) << "\","
        << "\"updated_register_count\":"
        << static_cast<unsigned>(value.memory_effects.updated_register_count)
        << ",\"region\":\"" << memory_region_name(value.memory_effects.region) << "\""
        << "},\"accumulator_effects\":{\"reads_if_s_clear\":";
    emit_json_names(output, accumulator_names(value.accumulator_effects.reads_if_s_clear));
    output << ",\"reads_if_s_set\":";
    emit_json_names(output, accumulator_names(value.accumulator_effects.reads_if_s_set));
    output << ",\"writes_if_s_clear\":";
    emit_json_names(output, accumulator_names(value.accumulator_effects.writes_if_s_clear));
    output << ",\"writes_if_s_set\":";
    emit_json_names(output, accumulator_names(value.accumulator_effects.writes_if_s_set));
    output << "},\"delay_slot\":{\"role\":\"" << delay_role_name(value.delay_slot.role)
        << "\",\"counterpart_address\":";
    if (value.delay_slot.counterpart_address) {
        output << '"' << hex32(*value.delay_slot.counterpart_address) << '"';
    } else {
        output << "null";
    }
    output << "},\"privileged\":" << boolean_name(value.is_privileged) << '}';
}

}

std::string emit_ir_text(const std::span<const Function> functions) {
    const auto ordered_functions = sorted_functions(functions);
    std::ostringstream output;
    output << "katana-ir-v2\n";
    for (const auto* function : ordered_functions) {
        output << "function " << hex32(function->entry_address) << '\n'
            << "  direct_callees=";
        emit_text_addresses(output, sorted_values(function->direct_callees));
        output << " indirect_call_sites=";
        emit_text_addresses(output, sorted_values(function->indirect_call_sites));
        output << '\n';
        for (const auto* block : sorted_blocks(*function)) {
            output << "  block " << hex32(block->start_address) << " successors=";
            emit_text_addresses(output, sorted_values(block->successors));
            output << " indirect=" << boolean_name(block->has_indirect_successor) << '\n';
            for (const auto* instruction : sorted_instructions(*block)) {
                emit_text_instruction(output, *instruction);
            }
        }
    }
    return output.str();
}

std::string emit_ir_json(const std::span<const Function> functions) {
    const auto ordered_functions = sorted_functions(functions);
    std::ostringstream output;
    output << "{\"schema\":\"katana-ir-v2\",\"functions\":[";
    for (std::size_t function_index = 0u;
        function_index < ordered_functions.size(); ++function_index) {
        if (function_index != 0u) output << ',';
        const auto& function = *ordered_functions[function_index];
        output << "{\"entry_address\":\"" << hex32(function.entry_address)
            << "\",\"direct_callees\":";
        emit_json_addresses(output, sorted_values(function.direct_callees));
        output << ",\"indirect_call_sites\":";
        emit_json_addresses(output, sorted_values(function.indirect_call_sites));
        output << ",\"blocks\":[";
        const auto blocks = sorted_blocks(function);
        for (std::size_t block_index = 0u; block_index < blocks.size(); ++block_index) {
            if (block_index != 0u) output << ',';
            const auto& block = *blocks[block_index];
            output << "{\"start_address\":\"" << hex32(block.start_address)
                << "\",\"successors\":";
            emit_json_addresses(output, sorted_values(block.successors));
            output << ",\"has_indirect_successor\":"
                << boolean_name(block.has_indirect_successor)
                << ",\"instructions\":[";
            const auto instructions = sorted_instructions(block);
            for (std::size_t instruction_index = 0u;
                instruction_index < instructions.size(); ++instruction_index) {
                if (instruction_index != 0u) output << ',';
                emit_json_instruction(output, *instructions[instruction_index]);
            }
            output << "]}";
        }
        output << "]}";
    }
    output << "],\"report_version\":" << katana::io::json_report_version
        << ",\"report_type\":\"ir\",\"status\":\"success\"}\n";
    return output.str();
}

}
