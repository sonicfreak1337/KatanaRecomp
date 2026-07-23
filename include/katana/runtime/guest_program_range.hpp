#pragma once

#include "katana/runtime/block_guards.hpp"

#include <cstdint>

namespace katana::runtime {

// Physical executable backing expressed through any linear P0/P1/P2 guest alias.
// A matching instruction must fit completely inside the half-open byte range.
struct GuestProgramRange {
    std::uint32_t guest_start = 0u;
    std::uint32_t byte_size = 0u;

    [[nodiscard]] bool operator==(const GuestProgramRange&) const noexcept = default;
};

[[nodiscard]] bool valid_guest_program_range(GuestProgramRange range) noexcept;

// Resolves an immutable range once so a block-level checkpoint does not repeat range
// canonicalization. The inline direct path never enters the TLB walker; only P0/P3 addresses
// classified as Mapped under an active MMU use the out-of-line translation path.
class GuestProgramRangeMatcher final {
  public:
    explicit GuestProgramRangeMatcher(GuestProgramRange range) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }
    [[nodiscard]] bool contains_instruction(const CpuState& cpu,
                                            std::uint32_t instruction_address) const noexcept {
        if (!valid_ || (instruction_address & 1u) != 0u) return false;
        const auto segment = instruction_address >> 29u;
        if (segment == 4u || segment == 5u) {
            // The long pre-BootExecutable IP.BIN path executes through P2. Reject its physical
            // range before touching CpuState or RuntimeAddressSpace; only a possible hit needs
            // the privilege check.
            if (!contains_physical_instruction(
                    canonical_physical_address(instruction_address)))
                return false;
            return cpu.privileged_mode();
        }
        if ((instruction_address & 0xFC000000u) == sh4_on_chip_ram_address)
            return contains_physical_instruction(instruction_address);
        if (segment >= 7u ||
            (!cpu.privileged_mode() && instruction_address >= 0x80000000u))
            return false;
        if (!cpu.address_space)
            return contains_physical_instruction(
                canonical_physical_address(instruction_address));

        switch (cpu.address_space->instruction_translation_path(
            instruction_address, cpu.privileged_mode())) {
        case InstructionTranslationPath::Direct:
            return contains_physical_instruction(
                canonical_physical_address(instruction_address));
        case InstructionTranslationPath::Mapped:
            return contains_mapped_instruction(cpu, instruction_address);
        case InstructionTranslationPath::Invalid:
            return false;
        }
        return false;
    }

  private:
    [[nodiscard]] bool
    contains_physical_instruction(std::uint32_t physical_instruction) const noexcept {
        return physical_instruction >= physical_start_ &&
               physical_instruction <= physical_last_instruction_;
    }
    [[nodiscard]] bool contains_mapped_instruction(
        const CpuState& cpu,
        std::uint32_t instruction_address) const noexcept;

    std::uint32_t physical_start_ = 0u;
    std::uint32_t physical_last_instruction_ = 0u;
    bool valid_ = false;
};

// Translates the candidate as an instruction through the active guest address space and compares
// the resulting physical address with range. Translation failures are diagnostic misses: this
// function never enters a guest exception or mutates CpuState.
[[nodiscard]] bool guest_program_range_contains_instruction(
    const CpuState& cpu,
    std::uint32_t instruction_address,
    GuestProgramRange range) noexcept;

} // namespace katana::runtime
