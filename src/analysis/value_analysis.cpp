#include "katana/analysis/value_analysis.hpp"
#include "katana/analysis/code_address.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace katana::analysis {
namespace {

void clear_constants(RegisterConstants& state) {
    state.registers.fill(std::nullopt);
    state.sources.fill({});
}

void set_constant(RegisterConstants& state,
                  const std::uint8_t index,
                  const std::uint32_t value,
                  std::string source = "constant-register") {
    state.registers[index] = value;
    state.sources[index] = std::move(source);
}

void apply_local_transfer(RegisterConstants& state,
                          const katana::sh4::DisassemblyLine& line,
                          const katana::io::ExecutableImage* image) {
    const auto& instruction = line.instruction;
    switch (instruction.kind) {
    case katana::sh4::InstructionKind::Nop:
        return;
    case katana::sh4::InstructionKind::MovImmediate:
        set_constant(state,
                     instruction.destination_register,
                     static_cast<std::uint32_t>(instruction.immediate));
        return;
    case katana::sh4::InstructionKind::MovRegister:
        state.registers[instruction.destination_register] =
            state.registers[instruction.source_register];
        state.sources[instruction.destination_register] =
            state.sources[instruction.source_register];
        return;
    case katana::sh4::InstructionKind::MovWordLoadPcRelative:
    case katana::sh4::InstructionKind::MovLongLoadPcRelative: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovWordLoadPcRelative ? 2u : 4u;
        const auto base = width == 4u ? (line.address + 4u) & ~3u : line.address + 4u;
        const auto address = base + static_cast<std::uint32_t>(instruction.displacement);
        const auto* segment = image != nullptr ? image->find_segment(address, width) : nullptr;
        if (segment == nullptr) {
            state.registers[instruction.destination_register].reset();
            state.sources[instruction.destination_register].clear();
            return;
        }
        const auto offset = *segment->byte_offset(address);
        const auto value =
            width == 4u
                ? image->read_u32_le(address)
                : static_cast<std::uint32_t>(static_cast<std::int32_t>(
                      static_cast<std::int16_t>(katana::io::read_u16_le(segment->bytes, offset))));
        set_constant(state, instruction.destination_register, value, "pc-relative-literal");
        return;
    }
    case katana::sh4::InstructionKind::MoveAddressPcRelative:
        set_constant(state,
                     0u,
                     ((line.address + 4u) & ~3u) +
                         static_cast<std::uint32_t>(instruction.displacement),
                     "pc-relative-address");
        return;
    case katana::sh4::InstructionKind::AddImmediate:
        if (state.registers[instruction.destination_register].has_value()) {
            *state.registers[instruction.destination_register] +=
                static_cast<std::uint32_t>(instruction.immediate);
        }
        return;
    case katana::sh4::InstructionKind::AddRegister:
        if (state.registers[instruction.destination_register].has_value() &&
            state.registers[instruction.source_register].has_value()) {
            *state.registers[instruction.destination_register] +=
                *state.registers[instruction.source_register];
        } else {
            state.registers[instruction.destination_register].reset();
            state.sources[instruction.destination_register].clear();
        }
        return;
    case katana::sh4::InstructionKind::SubRegister:
        if (state.registers[instruction.destination_register].has_value() &&
            state.registers[instruction.source_register].has_value()) {
            *state.registers[instruction.destination_register] -=
                *state.registers[instruction.source_register];
        } else {
            state.registers[instruction.destination_register].reset();
            state.sources[instruction.destination_register].clear();
        }
        return;
    case katana::sh4::InstructionKind::AndRegister:
    case katana::sh4::InstructionKind::OrRegister:
    case katana::sh4::InstructionKind::XorRegister:
        if (state.registers[instruction.destination_register].has_value() &&
            state.registers[instruction.source_register].has_value()) {
            auto& destination = *state.registers[instruction.destination_register];
            const auto source = *state.registers[instruction.source_register];
            if (instruction.kind == katana::sh4::InstructionKind::AndRegister) {
                destination &= source;
            } else if (instruction.kind == katana::sh4::InstructionKind::OrRegister) {
                destination |= source;
            } else {
                destination ^= source;
            }
        } else {
            state.registers[instruction.destination_register].reset();
            state.sources[instruction.destination_register].clear();
        }
        return;
    case katana::sh4::InstructionKind::AndImmediate:
    case katana::sh4::InstructionKind::OrImmediate:
    case katana::sh4::InstructionKind::XorImmediate:
        if (state.registers[0].has_value()) {
            auto& destination = *state.registers[0];
            const auto immediate = static_cast<std::uint32_t>(instruction.immediate);
            if (instruction.kind == katana::sh4::InstructionKind::AndImmediate) {
                destination &= immediate;
            } else if (instruction.kind == katana::sh4::InstructionKind::OrImmediate) {
                destination |= immediate;
            } else {
                destination ^= immediate;
            }
        }
        return;
    case katana::sh4::InstructionKind::Jmp:
    case katana::sh4::InstructionKind::Jsr:
    case katana::sh4::InstructionKind::Rts:
        clear_constants(state);
        return;
    default:
        clear_constants(state);
        return;
    }
}

std::vector<ConstantTraceEntry>
propagate_constants(const std::span<const katana::sh4::DisassemblyLine> lines,
                    const RegisterConstants& initial,
                    const katana::io::ExecutableImage* image) {
    RegisterConstants state = initial;
    std::vector<ConstantTraceEntry> trace;
    trace.reserve(lines.size());
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (index != 0u && line.address != lines[index - 1u].address + 2u) {
            clear_constants(state);
        }
        ConstantTraceEntry entry;
        entry.address = line.address;
        entry.before = state;
        apply_local_transfer(state, line, image);
        entry.after = state;
        trace.push_back(std::move(entry));
    }
    return trace;
}

} // namespace

std::vector<ConstantTraceEntry>
propagate_local_constants(const std::span<const katana::sh4::DisassemblyLine> lines,
                          const RegisterConstants& initial) {
    return propagate_constants(lines, initial, nullptr);
}

RegisterValueAnalysis
analyze_register_values(const std::span<const katana::sh4::DisassemblyLine> lines,
                        const RegisterConstants& initial) {
    RegisterValueAnalysis analysis;
    analysis.trace = propagate_constants(lines, initial, nullptr);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].instruction.kind != katana::sh4::InstructionKind::Jmp &&
            lines[index].instruction.kind != katana::sh4::InstructionKind::Jsr) {
            continue;
        }
        const auto register_index = lines[index].instruction.branch_register;
        analysis.indirect_control_flow.push_back(
            {lines[index].address,
             register_index,
             analysis.trace[index].before.registers[register_index],
             analysis.trace[index].before.sources[register_index]});
    }
    return analysis;
}

std::vector<IndirectControlFlowResolution>
resolve_indirect_control_flow(const std::span<const katana::sh4::DisassemblyLine> lines,
                              const katana::io::ExecutableImage& image) {
    RegisterValueAnalysis values;
    values.trace = propagate_constants(lines, {}, &image);
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        if (lines[index].instruction.kind != katana::sh4::InstructionKind::Jmp &&
            lines[index].instruction.kind != katana::sh4::InstructionKind::Jsr)
            continue;
        const auto register_index = lines[index].instruction.branch_register;
        values.indirect_control_flow.push_back(
            {lines[index].address,
             register_index,
             values.trace[index].before.registers[register_index],
             values.trace[index].before.sources[register_index]});
    }
    std::vector<IndirectControlFlowResolution> resolutions;
    resolutions.reserve(values.indirect_control_flow.size());
    for (const auto& observation : values.indirect_control_flow) {
        const auto line =
            std::find_if(lines.begin(),
                         lines.end(),
                         [&observation](const katana::sh4::DisassemblyLine& candidate) {
                             return candidate.address == observation.instruction_address;
                         });
        IndirectControlFlowResolution resolution;
        resolution.instruction_address = observation.instruction_address;
        resolution.register_index = observation.register_index;
        resolution.kind =
            line != lines.end() && line->instruction.kind == katana::sh4::InstructionKind::Jsr
                ? IndirectControlFlowKind::Call
                : IndirectControlFlowKind::Jump;
        if (!observation.value.has_value()) {
            resolution.reason = "register-value-unknown";
            resolutions.push_back(std::move(resolution));
            continue;
        }
        const auto target = *observation.value;
        const auto validation = validate_committed_code_address(image, target);
        if (!validation.valid()) {
            resolution.reason = code_address_status_name(validation.status);
            resolutions.push_back(std::move(resolution));
            continue;
        }
        resolution.status = ResolutionStatus::Resolved;
        resolution.target = target;
        resolution.reason = observation.source.empty() ? "constant-register" : observation.source;
        resolutions.push_back(std::move(resolution));
    }
    return resolutions;
}

} // namespace katana::analysis
