#include "katana/analysis/value_analysis.hpp"
#include "katana/analysis/code_address.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

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
        state.sources[index] =
            std::string(prefix) +
            (state.sources[index].empty() ? "constant-register" : state.sources[index]);
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

std::string normalized_source(const std::string& source) {
    return source.empty() ? "constant-register" : source;
}

std::string combined_source(const std::string_view operation,
                            const std::string& destination,
                            const std::string& source) {
    return std::string(operation) + '(' + normalized_source(destination) + ',' +
           normalized_source(source) + ')';
}

std::string immediate_source(const std::string_view operation, const std::string& source) {
    return std::string(operation) + '(' + normalized_source(source) + ",immediate)";
}

constexpr std::uint16_t register_bit(const std::uint8_t index) {
    return static_cast<std::uint16_t>(1u << index);
}

struct SnapshotWrite {
    std::uint32_t address = 0u;
    std::size_t width = 0u;
};

struct EntrySnapshotState {
    bool active = false;
    std::vector<SnapshotWrite> writes;
};

bool ranges_overlap(const std::uint32_t left_address,
                    const std::size_t left_width,
                    const std::uint32_t right_address,
                    const std::size_t right_width) {
    const auto left_end = static_cast<std::uint64_t>(left_address) + left_width;
    const auto right_end = static_cast<std::uint64_t>(right_address) + right_width;
    return static_cast<std::uint64_t>(left_address) < right_end &&
           static_cast<std::uint64_t>(right_address) < left_end;
}

bool snapshot_range_unchanged(const EntrySnapshotState* snapshot,
                              const std::uint32_t address,
                              const std::size_t width) {
    return snapshot != nullptr && snapshot->active &&
           std::none_of(snapshot->writes.begin(), snapshot->writes.end(), [&](const auto& write) {
               return ranges_overlap(address, width, write.address, write.width);
           });
}

std::optional<std::uint32_t> read_committed_integer(const katana::io::ExecutableImage* image,
                                                    const std::uint32_t address,
                                                    const std::size_t width) {
    if (image == nullptr) return std::nullopt;
    const auto* segment = image->find_segment(address, width);
    if (segment == nullptr || !segment->permissions.readable) return std::nullopt;
    const auto offset = segment->byte_offset(address);
    if (!offset.has_value() || *offset > segment->bytes.size() ||
        width > segment->bytes.size() - *offset)
        return std::nullopt;
    switch (width) {
    case 1u:
        return static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int8_t>(segment->bytes[*offset])));
    case 2u:
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(
            static_cast<std::int16_t>(katana::io::read_u16_le(segment->bytes, *offset))));
    case 4u:
        return static_cast<std::uint32_t>(katana::io::read_u16_le(segment->bytes, *offset)) |
               (static_cast<std::uint32_t>(katana::io::read_u16_le(segment->bytes, *offset + 2u))
                << 16u);
    default:
        return std::nullopt;
    }
}

std::optional<std::uint32_t> read_immutable_integer(const katana::io::ExecutableImage* image,
                                                    const std::uint32_t address,
                                                    const std::size_t width,
                                                    const EntrySnapshotState* snapshot = nullptr) {
    if (image == nullptr) return std::nullopt;
    const auto* segment = image->find_segment(address, width);
    if (segment == nullptr ||
        (segment->permissions.writable && !snapshot_range_unchanged(snapshot, address, width)))
        return std::nullopt;
    return read_committed_integer(image, address, width);
}

void apply_immutable_load(RegisterConstants& state,
                          const katana::sh4::DecodedInstruction& instruction,
                          const katana::io::ExecutableImage* image,
                          const std::optional<std::uint32_t> address,
                          const std::size_t width) {
    const auto value =
        address.has_value() ? read_immutable_integer(image, *address, width) : std::nullopt;
    if (!value.has_value()) {
        clear_register(state, instruction.destination_register);
        return;
    }
    set_constant(state,
                 instruction.destination_register,
                 *value,
                 width == 4u ? "bounded-immutable-pointer" : "bounded-immutable-value");
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

std::optional<std::uint32_t> checked_address(const std::optional<std::uint32_t> base,
                                             const std::uint32_t offset) {
    if (!base.has_value() || *base > std::numeric_limits<std::uint32_t>::max() - offset)
        return std::nullopt;
    return *base + offset;
}

void record_snapshot_write(EntrySnapshotState& snapshot,
                           const RegisterConstants& state,
                           const katana::sh4::DecodedInstruction& instruction) {
    if (!snapshot.active) return;
    using K = katana::sh4::InstructionKind;
    std::optional<std::uint32_t> address;
    std::size_t width = 0u;
    switch (instruction.kind) {
    case K::MovByteStore:
    case K::MovWordStore:
    case K::MovLongStore:
        width = instruction.kind == K::MovByteStore   ? 1u
                : instruction.kind == K::MovWordStore ? 2u
                                                      : 4u;
        address = state.registers[instruction.destination_register];
        break;
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
    case K::MovLongStoreDisplacement:
        width = instruction.kind == K::MovByteStoreDisplacement   ? 1u
                : instruction.kind == K::MovWordStoreDisplacement ? 2u
                                                                  : 4u;
        address = checked_address(state.registers[instruction.destination_register],
                                  static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
    case K::MovLongStoreR0Indexed:
        width = instruction.kind == K::MovByteStoreR0Indexed   ? 1u
                : instruction.kind == K::MovWordStoreR0Indexed ? 2u
                                                               : 4u;
        address = checked_address(state.registers[instruction.destination_register],
                                  state.registers[0u].value_or(0u));
        if (!state.registers[0u].has_value()) address.reset();
        break;
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement:
        width = instruction.kind == K::MovByteStorePreDecrement   ? 1u
                : instruction.kind == K::MovWordStorePreDecrement ? 2u
                                                                  : 4u;
        if (const auto base = state.registers[instruction.destination_register];
            base.has_value() && *base >= width)
            address = *base - static_cast<std::uint32_t>(width);
        break;
    case K::TestByteImmediate:
    case K::AndByteImmediate:
    case K::XorByteImmediate:
    case K::OrByteImmediate:
    case K::TestAndSetByte:
    case K::MovByteStoreGbrDisplacement:
    case K::MovWordStoreGbrDisplacement:
    case K::MovLongStoreGbrDisplacement:
    case K::StoreSpecialRegisterPreDecrement:
    case K::FmovStore:
    case K::FmovStorePreDecrement:
    case K::FmovStoreR0Indexed:
    case K::Prefetch:
    case K::Unknown:
        snapshot.active = false;
        return;
    default:
        return;
    }
    if (!address.has_value()) {
        snapshot.active = false;
        return;
    }
    snapshot.writes.push_back({*address, width});
}

void apply_local_transfer(RegisterConstants& state,
                          const katana::sh4::DisassemblyLine& line,
                          const katana::io::ExecutableImage* image,
                          const EntrySnapshotState* snapshot = nullptr) {
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
        auto value = read_immutable_integer(image, address, width, snapshot);
        bool guarded = false;
        if (!value.has_value()) {
            const auto* segment = image == nullptr ? nullptr : image->find_segment(address, width);
            if (segment != nullptr && segment->permissions.writable) {
                value = read_committed_integer(image, address, width);
                guarded = value.has_value();
            }
        }
        if (!value.has_value()) {
            clear_register(state, instruction.destination_register);
            return;
        }
        const auto* segment = image == nullptr ? nullptr : image->find_segment(address, width);
        set_constant(state,
                     instruction.destination_register,
                     *value,
                     guarded ? "guarded-writable-pc-relative-literal"
                     : segment != nullptr && segment->permissions.writable
                         ? "entry-snapshot-pc-relative-literal"
                         : "pc-relative-literal");
        return;
    }
    case katana::sh4::InstructionKind::MoveAddressPcRelative:
        set_constant(state,
                     0u,
                     ((line.address + 4u) & ~3u) +
                         static_cast<std::uint32_t>(instruction.displacement),
                     "pc-relative-address");
        return;
    case katana::sh4::InstructionKind::MovByteLoad:
    case katana::sh4::InstructionKind::MovWordLoad:
    case katana::sh4::InstructionKind::MovLongLoad: {
        const auto width = instruction.kind == katana::sh4::InstructionKind::MovByteLoad   ? 1u
                           : instruction.kind == katana::sh4::InstructionKind::MovWordLoad ? 2u
                                                                                           : 4u;
        apply_immutable_load(
            state, instruction, image, state.registers[instruction.source_register], width);
        return;
    }
    case katana::sh4::InstructionKind::MovByteLoadDisplacement:
    case katana::sh4::InstructionKind::MovWordLoadDisplacement:
    case katana::sh4::InstructionKind::MovLongLoadDisplacement: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadDisplacement   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadDisplacement ? 2u
                                                                                        : 4u;
        const auto base = state.registers[instruction.source_register];
        const auto address = base.has_value()
                                 ? std::optional<std::uint32_t>(
                                       *base + static_cast<std::uint32_t>(instruction.displacement))
                                 : std::nullopt;
        apply_immutable_load(state, instruction, image, address, width);
        return;
    }
    case katana::sh4::InstructionKind::MovByteLoadR0Indexed:
    case katana::sh4::InstructionKind::MovWordLoadR0Indexed:
    case katana::sh4::InstructionKind::MovLongLoadR0Indexed: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadR0Indexed   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadR0Indexed ? 2u
                                                                                     : 4u;
        const auto base = state.registers[0u];
        const auto index = state.registers[instruction.source_register];
        const auto address = base.has_value() && index.has_value()
                                 ? std::optional<std::uint32_t>(*base + *index)
                                 : std::nullopt;
        apply_immutable_load(state, instruction, image, address, width);
        return;
    }
    case katana::sh4::InstructionKind::AddImmediate:
        if (state.registers[instruction.destination_register].has_value()) {
            *state.registers[instruction.destination_register] +=
                static_cast<std::uint32_t>(instruction.immediate);
            state.sources[instruction.destination_register] =
                immediate_source("add", state.sources[instruction.destination_register]);
        }
        return;
    case katana::sh4::InstructionKind::AddRegister:
        if (state.registers[instruction.destination_register].has_value() &&
            state.registers[instruction.source_register].has_value()) {
            const auto provenance = combined_source("add",
                                                    state.sources[instruction.destination_register],
                                                    state.sources[instruction.source_register]);
            *state.registers[instruction.destination_register] +=
                *state.registers[instruction.source_register];
            state.sources[instruction.destination_register] = provenance;
        } else {
            clear_register(state, instruction.destination_register);
        }
        return;
    case katana::sh4::InstructionKind::SubRegister:
        if (instruction.destination_register == instruction.source_register) {
            set_constant(state, instruction.destination_register, 0u, "sub-self");
            return;
        }
        if (state.registers[instruction.destination_register].has_value() &&
            state.registers[instruction.source_register].has_value()) {
            const auto provenance = combined_source("sub",
                                                    state.sources[instruction.destination_register],
                                                    state.sources[instruction.source_register]);
            *state.registers[instruction.destination_register] -=
                *state.registers[instruction.source_register];
            state.sources[instruction.destination_register] = provenance;
        } else {
            clear_register(state, instruction.destination_register);
        }
        return;
    case katana::sh4::InstructionKind::AndRegister:
    case katana::sh4::InstructionKind::OrRegister:
    case katana::sh4::InstructionKind::XorRegister:
        if (instruction.kind == katana::sh4::InstructionKind::XorRegister &&
            instruction.destination_register == instruction.source_register) {
            set_constant(state, instruction.destination_register, 0u, "xor-self");
            return;
        }
        if (state.registers[instruction.destination_register].has_value() &&
            state.registers[instruction.source_register].has_value()) {
            const auto operation =
                instruction.kind == katana::sh4::InstructionKind::AndRegister  ? "and"
                : instruction.kind == katana::sh4::InstructionKind::OrRegister ? "or"
                                                                               : "xor";
            const auto provenance = combined_source(operation,
                                                    state.sources[instruction.destination_register],
                                                    state.sources[instruction.source_register]);
            auto& destination = *state.registers[instruction.destination_register];
            const auto source = *state.registers[instruction.source_register];
            if (instruction.kind == katana::sh4::InstructionKind::AndRegister) {
                destination &= source;
            } else if (instruction.kind == katana::sh4::InstructionKind::OrRegister) {
                destination |= source;
            } else {
                destination ^= source;
            }
            state.sources[instruction.destination_register] = provenance;
        } else {
            clear_register(state, instruction.destination_register);
        }
        return;
    case katana::sh4::InstructionKind::AndImmediate:
    case katana::sh4::InstructionKind::OrImmediate:
    case katana::sh4::InstructionKind::XorImmediate:
        if (state.registers[0].has_value()) {
            const auto operation =
                instruction.kind == katana::sh4::InstructionKind::AndImmediate  ? "and"
                : instruction.kind == katana::sh4::InstructionKind::OrImmediate ? "or"
                                                                                : "xor";
            auto& destination = *state.registers[0];
            const auto immediate = static_cast<std::uint32_t>(instruction.immediate);
            if (instruction.kind == katana::sh4::InstructionKind::AndImmediate) {
                destination &= immediate;
            } else if (instruction.kind == katana::sh4::InstructionKind::OrImmediate) {
                destination |= immediate;
            } else {
                destination ^= immediate;
            }
            state.sources[0] = immediate_source(operation, state.sources[0]);
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
            if (line.instruction.control_flow == katana::sh4::ControlFlowKind::ConditionalBranch)
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

std::uint16_t
general_register_write_mask(const katana::sh4::DecodedInstruction& instruction) noexcept {
    return general_register_writes(instruction);
}

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
    std::vector<IndirectControlFlowResolution> resolutions;
    RegisterConstants state;
    std::set<std::uint32_t> control_flow_targets;
    for (const auto& line : lines) {
        if (line.target_address.has_value()) control_flow_targets.insert(*line.target_address);
    }
    std::optional<std::uint16_t> clear_after_delay_slot;
    EntrySnapshotState snapshot;
    snapshot.active = image.initial_snapshot_policy() ==
                          katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent &&
                      image.entry_points().size() == 1u && !lines.empty() &&
                      lines.front().address == image.entry_points().front();
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (index != 0u && line.address != lines[index - 1u].address + 2u) {
            clear_constants(state);
            clear_after_delay_slot.reset();
            snapshot.active = false;
        }
        if (control_flow_targets.contains(line.address)) {
            clear_constants(state);
            snapshot.active = false;
        }

        const bool indirect = line.instruction.kind == katana::sh4::InstructionKind::Jmp ||
                              line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                              line.instruction.kind == katana::sh4::InstructionKind::Braf ||
                              line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
        if (indirect) {
            const auto register_index = line.instruction.branch_register;
            const auto& value = state.registers[register_index];
            const auto& source = state.sources[register_index];
            IndirectControlFlowResolution resolution;
            resolution.instruction_address = line.address;
            resolution.register_index = register_index;
            resolution.kind = line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                                      line.instruction.kind == katana::sh4::InstructionKind::Bsrf
                                  ? IndirectControlFlowKind::Call
                                  : IndirectControlFlowKind::Jump;
            if (!value.has_value()) {
                resolution.reason = "register-value-unknown";
            } else {
                const bool register_relative =
                    line.instruction.kind == katana::sh4::InstructionKind::Braf ||
                    line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
                const auto narrowed_target = register_relative
                                                 ? static_cast<std::uint32_t>(
                                                       *value + line.address + 4u)
                                                 : *value;
                {
                    const auto validation = validate_decode_candidate(image, narrowed_target);
                    if (!validation.valid()) {
                        resolution.reason = code_address_status_name(validation.status);
                    } else {
                        resolution.status = source.find("guarded-writable-") != std::string::npos
                                                ? ResolutionStatus::Guarded
                                                : ResolutionStatus::Resolved;
                        resolution.evidence =
                            resolution.status == ResolutionStatus::Resolved
                                ? ControlFlowEvidence::ProvenComplete
                                : ControlFlowEvidence::GuardedPartial;
                        resolution.evidence_origins = {
                            resolution.status == ResolutionStatus::Resolved
                                ? AnalysisEvidenceOrigin::LocalValue
                                : AnalysisEvidenceOrigin::EntrySnapshot};
                        resolution.target = narrowed_target;
                        resolution.reason = source.empty() ? "constant-register" : source;
                        if (register_relative)
                            resolution.reason = "register-relative-" + resolution.reason;
                    }
                }
            }
            resolutions.push_back(std::move(resolution));
        }

        record_snapshot_write(snapshot, state, line.instruction);
        apply_local_transfer(state, line, &image, &snapshot);
        if (clear_after_delay_slot.has_value()) {
            clear_registers(state, *clear_after_delay_slot);
            if (*clear_after_delay_slot == 0x80FFu) mark_abi_preserved(state, 0x7F00u);
            clear_after_delay_slot.reset();
        }
        if (!line.instruction.changes_control_flow() ||
            line.instruction.control_flow == katana::sh4::ControlFlowKind::ConditionalBranch) {
            if (line.instruction.control_flow == katana::sh4::ControlFlowKind::ConditionalBranch)
                snapshot.active = false;
            continue;
        }
        snapshot.active = false;
        const bool call =
            line.instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
            line.instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall;
        const auto clear_mask = call && image.guest_call_abi() == katana::io::GuestCallAbi::SuperHC
                                    ? static_cast<std::uint16_t>(0x80FFu)
                                    : static_cast<std::uint16_t>(0xFFFFu);
        if (line.instruction.has_delay_slot) {
            clear_after_delay_slot = clear_mask;
        } else {
            clear_registers(state, clear_mask);
            if (clear_mask == 0x80FFu) mark_abi_preserved(state, 0x7F00u);
        }
    }
    return resolutions;
}

} // namespace katana::analysis
