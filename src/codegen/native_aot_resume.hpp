#pragma once

#include "katana/ir/ir.hpp"
#include "katana/sh4/instruction_timing.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace katana::codegen::detail {

[[nodiscard]] inline bool native_aot_requires_architectural_resume(
    const katana::ir::Instruction& instruction) noexcept {
    using Operation = katana::ir::Operation;
    using Register = katana::ir::SpecialRegister;

    if ((instruction.operation == Operation::LoadSpecialRegister ||
         instruction.operation == Operation::LoadSpecialRegisterPostIncrement) &&
        (instruction.special_register == Register::Sr ||
         instruction.special_register == Register::Fpscr))
        return true;

    return instruction.operation == Operation::LoadTlb ||
           instruction.operation == Operation::Frchg ||
           instruction.operation == Operation::Fschg;
}

[[nodiscard]] inline bool native_aot_has_proven_linear_ram_access(
    const katana::ir::Instruction& instruction) noexcept {
    return instruction.memory_effects.access != katana::ir::MemoryAccessKind::None &&
           instruction.memory_effects.region == katana::ir::MemoryRegionKind::NormalRam;
}

// A Product AOT block may defer the ordinary memory boundary for these loads
// only after the generated code has proved the concrete address against the
// current direct-linear main-RAM guard. Direct P1/P2 aliases and exact No-MMU
// aliases are admitted; every other operation and every failed runtime proof
// retains the generic Memory/MMU/MMIO path.
[[nodiscard]] inline bool native_aot_has_guarded_linear_ram_read(
    const katana::ir::Instruction& instruction) noexcept {
    using Operation = katana::ir::Operation;

    if (instruction.memory_effects.access != katana::ir::MemoryAccessKind::Read ||
        instruction.memory_effects.region != katana::ir::MemoryRegionKind::Unknown)
        return false;

    switch (instruction.operation) {
    case Operation::FmovLoad:
    case Operation::FmovLoadPostIncrement:
    case Operation::FmovLoadR0Indexed:
    case Operation::MultiplyAccumulateWord:
    case Operation::MultiplyAccumulateLong:
    case Operation::TestByteImmediate:
    case Operation::LoadByteSigned:
    case Operation::LoadWordSigned:
    case Operation::LoadLong:
    case Operation::LoadByteSignedPostIncrement:
    case Operation::LoadWordSignedPostIncrement:
    case Operation::LoadLongPostIncrement:
    case Operation::LoadByteSignedDisplacement:
    case Operation::LoadWordSignedDisplacement:
    case Operation::LoadLongDisplacement:
    case Operation::LoadByteSignedR0Indexed:
    case Operation::LoadWordSignedR0Indexed:
    case Operation::LoadLongR0Indexed:
    case Operation::LoadByteSignedGbrDisplacement:
    case Operation::LoadWordSignedGbrDisplacement:
    case Operation::LoadLongGbrDisplacement:
    case Operation::LoadWordSignedPcRelative:
    case Operation::LoadLongPcRelative:
    case Operation::LoadSpecialRegisterPostIncrement:
        return true;
    default:
        return false;
    }
}

// Unknown-region stores retain the generic translated/MMIO path unless the
// generated instruction first proves its complete effective address against
// the current direct main-RAM window. The scalar helper still notifies the
// executable-code observer and requests an exact fallthrough exit whenever a
// write aliases live code.
[[nodiscard]] inline bool native_aot_has_guarded_linear_ram_write(
    const katana::ir::Instruction& instruction) noexcept {
    using Operation = katana::ir::Operation;

    if (instruction.memory_effects.access != katana::ir::MemoryAccessKind::Write ||
        instruction.memory_effects.region != katana::ir::MemoryRegionKind::Unknown)
        return false;

    switch (instruction.operation) {
    case Operation::StoreByte:
    case Operation::StoreWord:
    case Operation::StoreLong:
    case Operation::StoreBytePreDecrement:
    case Operation::StoreWordPreDecrement:
    case Operation::StoreLongPreDecrement:
    case Operation::StoreByteDisplacement:
    case Operation::StoreWordDisplacement:
    case Operation::StoreLongDisplacement:
    case Operation::StoreByteR0Indexed:
    case Operation::StoreWordR0Indexed:
    case Operation::StoreLongR0Indexed:
    case Operation::StoreByteGbrDisplacement:
    case Operation::StoreWordGbrDisplacement:
    case Operation::StoreLongGbrDisplacement:
    case Operation::StoreSpecialRegisterPreDecrement:
    case Operation::MovcaLong:
    case Operation::FmovStore:
    case Operation::FmovStorePreDecrement:
    case Operation::FmovStoreR0Indexed:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool native_aot_has_guarded_linear_ram_access(
    const katana::ir::Instruction& instruction) noexcept {
    return native_aot_has_guarded_linear_ram_read(instruction) ||
           native_aot_has_guarded_linear_ram_write(instruction);
}

[[nodiscard]] inline bool native_aot_requires_direct_write_resume(
    const katana::ir::Instruction& instruction) noexcept {
    // Every emitted guest store observes the immutable AOT-code contract.
    // Even an otherwise generic/MMIO-capable write can therefore request an
    // exact post-instruction exit when it changes executable bytes.
    return instruction.delay_slot.role ==
               katana::ir::DelaySlotRole::None &&
           instruction.memory_effects.access ==
               katana::ir::MemoryAccessKind::Write;
}

[[nodiscard]] inline bool native_aot_may_return_at_instruction_fallthrough(
    const katana::ir::Instruction& instruction) noexcept {
    if (instruction.delay_slot.role != katana::ir::DelaySlotRole::None) return false;

    const auto timing = katana::sh4::instruction_timing(instruction.original_opcode);
    const bool possible_mmio_boundary =
        timing.requires_cycle_flush && !native_aot_has_proven_linear_ram_access(instruction);
    const bool direct_code_write_exit =
        native_aot_requires_direct_write_resume(instruction);
    return native_aot_requires_architectural_resume(instruction) ||
           possible_mmio_boundary || direct_code_write_exit;
}

// Native AOT can yield after an instruction when a scheduler/MMIO boundary or
// executable-code invalidation is observed. If the architectural fallthrough
// is still inside the same IR block, that halfword is an exact resume entry,
// not a new function and not an arbitrary mid-block dispatch target.
[[nodiscard]] inline std::vector<std::uint32_t>
native_aot_internal_resume_entries(const katana::ir::BasicBlock& block) {
    std::vector<std::uint32_t> entries;
    entries.reserve(block.instructions.size());

    const auto strictly_increasing_addresses =
        std::adjacent_find(
            block.instructions.begin(),
            block.instructions.end(),
            [](const auto& left, const auto& right) {
                return left.source_address >= right.source_address;
            }) == block.instructions.end();

    // Validated IR stores a block in strictly ascending address order. Walk
    // that order once instead of searching the complete block again for every
    // possible resume. Keep the original all-instruction search as an exact
    // fallback for malformed/pre-validation input so this optimization cannot
    // hide the diagnostics that reject it later.
    std::size_t local_instruction_index = 0u;
    for (const auto& instruction : block.instructions) {
        if (!native_aot_may_return_at_instruction_fallthrough(instruction) ||
            instruction.source_address >
                std::numeric_limits<std::uint32_t>::max() - 2u)
            continue;
        const auto resume = instruction.source_address + 2u;
        if (resume == block.start_address) continue;
        bool is_local_instruction = false;
        if (strictly_increasing_addresses) {
            while (local_instruction_index < block.instructions.size() &&
                   block.instructions[local_instruction_index].source_address <
                       resume)
                ++local_instruction_index;
            is_local_instruction =
                local_instruction_index < block.instructions.size() &&
                block.instructions[local_instruction_index].source_address ==
                    resume &&
                block.instructions[local_instruction_index].delay_slot.role !=
                    katana::ir::DelaySlotRole::Slot;
        } else {
            is_local_instruction =
                std::any_of(block.instructions.begin(),
                            block.instructions.end(),
                            [&](const auto& candidate) {
                                return candidate.source_address == resume &&
                                       candidate.delay_slot.role !=
                                           katana::ir::DelaySlotRole::Slot;
                            });
        }
        if (is_local_instruction) entries.push_back(resume);
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    return entries;
}

} // namespace katana::codegen::detail
