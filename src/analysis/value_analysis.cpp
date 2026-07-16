#include "katana/analysis/value_analysis.hpp"
#include "katana/analysis/code_address.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace katana::analysis {
namespace {

void clear_constants(RegisterConstants& state) {
    state.registers.fill(std::nullopt);
    state.sources.fill({});
}

void clear_registers(RegisterConstants& state, const std::uint16_t mask) {
    for (std::uint8_t index = 0u; index < state.registers.size(); ++index) {
        if ((mask & static_cast<std::uint16_t>(1u << index)) == 0u) continue;
        state.registers[index].reset();
        state.sources[index].clear();
    }
}

void mark_abi_preserved(RegisterConstants& state, const std::uint16_t preserved_mask) {
    constexpr std::string_view prefix = "sh-c-abi-preserved-";
    for (std::uint8_t index = 0u; index < state.registers.size(); ++index) {
        if ((preserved_mask & static_cast<std::uint16_t>(1u << index)) == 0u ||
            !state.registers[index].has_value() || state.sources[index].starts_with(prefix))
            continue;
        state.sources[index] = std::string(prefix) +
                               (state.sources[index].empty() ? "constant-register"
                                                             : state.sources[index]);
    }
}

void set_constant(RegisterConstants& state,
                  const std::uint8_t index,
                  const std::uint32_t value,
                  std::string source = "constant-register") {
    state.registers[index] = value;
    state.sources[index] = std::move(source);
}

void clear_register(RegisterConstants& state, const std::uint8_t index) {
    state.registers[index].reset();
    state.sources[index].clear();
}

constexpr std::uint16_t register_bit(const std::uint8_t index) {
    return static_cast<std::uint16_t>(1u << index);
}

std::uint16_t general_register_writes(const katana::sh4::DecodedInstruction& instruction) {
    using K = katana::sh4::InstructionKind;
    switch (instruction.kind) {
    case K::MovImmediate:
    case K::AddImmediate:
    case K::MovRegister:
    case K::AddRegister:
    case K::SubRegister:
    case K::NegateRegister:
    case K::NotRegister:
    case K::AddWithCarry:
    case K::AddWithOverflow:
    case K::SubWithCarry:
    case K::SubWithOverflow:
    case K::NegateWithCarry:
    case K::ExtendUnsignedByte:
    case K::ExtendUnsignedWord:
    case K::ExtendSignedByte:
    case K::ExtendSignedWord:
    case K::SwapBytes:
    case K::SwapWords:
    case K::ExtractMiddle:
    case K::DecrementAndTest:
    case K::MoveT:
    case K::ShiftLogicalLeftOne:
    case K::ShiftLogicalRightOne:
    case K::ShiftArithmeticLeftOne:
    case K::ShiftArithmeticRightOne:
    case K::ShiftLogicalLeftTwo:
    case K::ShiftLogicalLeftEight:
    case K::ShiftLogicalLeftSixteen:
    case K::ShiftLogicalRightTwo:
    case K::ShiftLogicalRightEight:
    case K::ShiftLogicalRightSixteen:
    case K::RotateLeft:
    case K::RotateRight:
    case K::RotateLeftThroughT:
    case K::RotateRightThroughT:
    case K::ShiftArithmeticDynamic:
    case K::ShiftLogicalDynamic:
    case K::AndRegister:
    case K::OrRegister:
    case K::XorRegister:
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
    case K::MovByteLoadGbrDisplacement:
    case K::MovWordLoadGbrDisplacement:
    case K::MovLongLoadGbrDisplacement:
    case K::MovWordLoadPcRelative:
    case K::MovLongLoadPcRelative:
    case K::StoreSpecialRegister:
        return register_bit(instruction.destination_register);

    case K::AndImmediate:
    case K::OrImmediate:
    case K::XorImmediate:
    case K::MoveAddressPcRelative:
        return register_bit(0u);

    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement:
    case K::StoreSpecialRegisterPreDecrement:
    case K::FmovStorePreDecrement:
        return register_bit(instruction.destination_register);

    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
        return static_cast<std::uint16_t>(register_bit(instruction.destination_register) |
                                          register_bit(instruction.source_register));

    case K::LoadSpecialRegisterPostIncrement:
    case K::FmovLoadPostIncrement:
        return register_bit(instruction.source_register);

    case K::MultiplyAccumulateWord:
    case K::MultiplyAccumulateLong:
        return static_cast<std::uint16_t>(register_bit(instruction.destination_register) |
                                          register_bit(instruction.source_register));

    case K::Unknown:
        return 0xFFFFu;

    default:
        return 0u;
    }
}

void clear_written_registers(RegisterConstants& state,
                             const katana::sh4::DecodedInstruction& instruction) {
    const auto writes = general_register_writes(instruction);
    for (std::uint8_t index = 0u; index < state.registers.size(); ++index) {
        if ((writes & register_bit(index)) != 0u) clear_register(state, index);
    }
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
            clear_register(state, instruction.destination_register);
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
            clear_register(state, instruction.destination_register);
        }
        return;
    case katana::sh4::InstructionKind::SubRegister:
        if (state.registers[instruction.destination_register].has_value() &&
            state.registers[instruction.source_register].has_value()) {
            *state.registers[instruction.destination_register] -=
                *state.registers[instruction.source_register];
        } else {
            clear_register(state, instruction.destination_register);
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
            clear_register(state, instruction.destination_register);
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
    default:
        clear_written_registers(state, instruction);
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
    std::set<std::uint32_t> control_flow_targets;
    for (const auto& line : lines) {
        if (line.target_address.has_value()) control_flow_targets.insert(*line.target_address);
    }
    std::optional<std::uint16_t> clear_after_delay_slot;
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (index != 0u && line.address != lines[index - 1u].address + 2u) {
            clear_constants(state);
            clear_after_delay_slot.reset();
        }
        if (control_flow_targets.contains(line.address)) clear_constants(state);
        ConstantTraceEntry entry;
        entry.address = line.address;
        entry.before = state;
        apply_local_transfer(state, line, image);
        entry.after = state;
        trace.push_back(std::move(entry));
        if (clear_after_delay_slot.has_value()) {
            clear_registers(state, *clear_after_delay_slot);
            if (*clear_after_delay_slot == 0x80FFu) mark_abi_preserved(state, 0x7F00u);
            clear_after_delay_slot.reset();
        }
        if (line.instruction.changes_control_flow()) {
            if (line.instruction.control_flow ==
                katana::sh4::ControlFlowKind::ConditionalBranch)
                continue;
            const bool call =
                line.instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall;
            const auto clear_mask =
                call && image != nullptr &&
                        image->guest_call_abi() == katana::io::GuestCallAbi::SuperHC
                    ? static_cast<std::uint16_t>(0x80FFu)
                    : static_cast<std::uint16_t>(0xFFFFu);
            if (line.instruction.has_delay_slot) {
                clear_after_delay_slot = clear_mask;
            } else {
                clear_registers(state, clear_mask);
                if (clear_mask == 0x80FFu) mark_abi_preserved(state, 0x7F00u);
            }
        }
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
            lines[index].instruction.kind != katana::sh4::InstructionKind::Jsr &&
            lines[index].instruction.kind != katana::sh4::InstructionKind::Braf &&
            lines[index].instruction.kind != katana::sh4::InstructionKind::Bsrf) {
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
            lines[index].instruction.kind != katana::sh4::InstructionKind::Jsr &&
            lines[index].instruction.kind != katana::sh4::InstructionKind::Braf &&
            lines[index].instruction.kind != katana::sh4::InstructionKind::Bsrf)
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
            line != lines.end() && (line->instruction.kind == katana::sh4::InstructionKind::Jsr ||
                                    line->instruction.kind == katana::sh4::InstructionKind::Bsrf)
                ? IndirectControlFlowKind::Call
                : IndirectControlFlowKind::Jump;
        if (!observation.value.has_value()) {
            resolution.reason = "register-value-unknown";
            resolutions.push_back(std::move(resolution));
            continue;
        }
        auto target = *observation.value;
        const bool register_relative =
            line != lines.end() && (line->instruction.kind == katana::sh4::InstructionKind::Braf ||
                                    line->instruction.kind == katana::sh4::InstructionKind::Bsrf);
        if (register_relative) target += observation.instruction_address + 4u;
        const auto validation = validate_committed_code_address(image, target);
        if (!validation.valid()) {
            resolution.reason = code_address_status_name(validation.status);
            resolutions.push_back(std::move(resolution));
            continue;
        }
        resolution.status = ResolutionStatus::Resolved;
        resolution.target = target;
        resolution.reason = observation.source.empty() ? "constant-register" : observation.source;
        if (register_relative) resolution.reason = "register-relative-" + resolution.reason;
        resolutions.push_back(std::move(resolution));
    }
    return resolutions;
}

} // namespace katana::analysis
