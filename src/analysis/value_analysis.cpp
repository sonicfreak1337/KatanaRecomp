#include "katana/analysis/value_analysis.hpp"

#include "katana/sh4/instruction.hpp"

#include <cstdint>

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

}
