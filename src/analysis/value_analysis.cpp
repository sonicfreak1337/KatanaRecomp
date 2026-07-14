#include "katana/analysis/value_analysis.hpp"

#include "katana/sh4/instruction.hpp"

#include <cstdint>
#include <algorithm>

namespace katana::analysis {
namespace {

void apply_local_transfer(
    RegisterConstants& state,
    const katana::sh4::DecodedInstruction& instruction
) {
    switch (instruction.kind) {
        case katana::sh4::InstructionKind::Nop:
            return;
        case katana::sh4::InstructionKind::MovImmediate:
            state.registers[instruction.destination_register] =
                static_cast<std::uint32_t>(instruction.immediate);
            return;
        case katana::sh4::InstructionKind::MovRegister:
            state.registers[instruction.destination_register] =
                state.registers[instruction.source_register];
            return;
        case katana::sh4::InstructionKind::AddImmediate:
            if (state.registers[instruction.destination_register].has_value()) {
                *state.registers[instruction.destination_register] +=
                    static_cast<std::uint32_t>(instruction.immediate);
            }
            return;
        case katana::sh4::InstructionKind::AddRegister:
            if (state.registers[instruction.destination_register].has_value()
                && state.registers[instruction.source_register].has_value()) {
                *state.registers[instruction.destination_register] +=
                    *state.registers[instruction.source_register];
            } else {
                state.registers[instruction.destination_register].reset();
            }
            return;
        case katana::sh4::InstructionKind::SubRegister:
            if (state.registers[instruction.destination_register].has_value()
                && state.registers[instruction.source_register].has_value()) {
                *state.registers[instruction.destination_register] -=
                    *state.registers[instruction.source_register];
            } else {
                state.registers[instruction.destination_register].reset();
            }
            return;
        case katana::sh4::InstructionKind::AndRegister:
        case katana::sh4::InstructionKind::OrRegister:
        case katana::sh4::InstructionKind::XorRegister:
            if (state.registers[instruction.destination_register].has_value()
                && state.registers[instruction.source_register].has_value()) {
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
            state.registers.fill(std::nullopt);
            return;
        default:
            state.registers.fill(std::nullopt);
            return;
    }
}

}

std::vector<ConstantTraceEntry> propagate_local_constants(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const RegisterConstants& initial
) {
    RegisterConstants state = initial;
    std::vector<ConstantTraceEntry> trace;
    trace.reserve(lines.size());
    for (const auto& line : lines) {
        ConstantTraceEntry entry;
        entry.address = line.address;
        entry.before = state;
        apply_local_transfer(state, line.instruction);
        entry.after = state;
        trace.push_back(std::move(entry));
    }
    return trace;
}

RegisterValueAnalysis analyze_register_values(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const RegisterConstants& initial
) {
    RegisterValueAnalysis analysis;
    analysis.trace = propagate_local_constants(lines, initial);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].instruction.kind != katana::sh4::InstructionKind::Jmp
            && lines[index].instruction.kind != katana::sh4::InstructionKind::Jsr) {
            continue;
        }
        const auto register_index = lines[index].instruction.branch_register;
        analysis.indirect_control_flow.push_back({
            lines[index].address,
            register_index,
            analysis.trace[index].before.registers[register_index]
        });
    }
    return analysis;
}

std::vector<IndirectControlFlowResolution> resolve_indirect_control_flow(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::io::ExecutableImage& image
) {
    const auto values = analyze_register_values(lines);
    std::vector<IndirectControlFlowResolution> resolutions;
    resolutions.reserve(values.indirect_control_flow.size());
    for (const auto& observation : values.indirect_control_flow) {
        const auto line = std::find_if(
            lines.begin(),
            lines.end(),
            [&observation](const katana::sh4::DisassemblyLine& candidate) {
                return candidate.address == observation.instruction_address;
            }
        );
        IndirectControlFlowResolution resolution;
        resolution.instruction_address = observation.instruction_address;
        resolution.register_index = observation.register_index;
        resolution.kind = line != lines.end()
                && line->instruction.kind == katana::sh4::InstructionKind::Jsr
            ? IndirectControlFlowKind::Call
            : IndirectControlFlowKind::Jump;
        if (!observation.value.has_value()) {
            resolution.reason = "register-value-unknown";
            resolutions.push_back(std::move(resolution));
            continue;
        }
        const auto target = *observation.value;
        const auto* segment = image.find_segment(target, 2u);
        const auto byte_offset = segment == nullptr ? std::optional<std::size_t>{}
                                                    : segment->byte_offset(target);
        if ((target & 1u) != 0u || segment == nullptr
            || segment->kind != katana::io::SegmentKind::Code
            || !segment->permissions.executable || !byte_offset.has_value()
            || segment->bytes.size() < 2u || *byte_offset > segment->bytes.size() - 2u) {
            resolution.reason = "target-not-committed-executable-code";
            resolutions.push_back(std::move(resolution));
            continue;
        }
        resolution.status = ResolutionStatus::Resolved;
        resolution.target = target;
        resolution.reason = "constant-register";
        resolutions.push_back(std::move(resolution));
    }
    return resolutions;
}

}
