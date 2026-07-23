#include "katana/runtime/guest_program_range.hpp"

#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/block_table.hpp"

#include <cstdint>
#include <limits>

namespace katana::runtime {
namespace {

constexpr std::uint64_t guest_address_space_size =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;

bool resolve_range(const GuestProgramRange range,
                   std::uint32_t& physical_start,
                   std::uint64_t& physical_end) noexcept {
    if ((range.guest_start & 1u) != 0u || range.byte_size < 2u) return false;
    const auto virtual_end =
        static_cast<std::uint64_t>(range.guest_start) + range.byte_size;
    if (virtual_end > guest_address_space_size) return false;

    physical_start = canonical_physical_address(range.guest_start);
    physical_end = static_cast<std::uint64_t>(physical_start) + range.byte_size;
    if (physical_end > guest_address_space_size) return false;

    const auto virtual_last = static_cast<std::uint32_t>(virtual_end - 1u);
    return canonical_physical_address(virtual_last) == physical_end - 1u;
}

} // namespace

bool valid_guest_program_range(const GuestProgramRange range) noexcept {
    return GuestProgramRangeMatcher(range).valid();
}

GuestProgramRangeMatcher::GuestProgramRangeMatcher(const GuestProgramRange range) noexcept {
    std::uint64_t physical_end = 0u;
    valid_ = resolve_range(range, physical_start_, physical_end);
    if (valid_)
        physical_last_instruction_ = static_cast<std::uint32_t>(physical_end - 2u);
}

bool GuestProgramRangeMatcher::contains_mapped_instruction(
    const CpuState& cpu,
    const std::uint32_t instruction_address) const noexcept {
    try {
        const auto physical_instruction =
            cpu.address_space
                ->translate(instruction_address,
                            TranslationAccess::Instruction,
                            cpu.privileged_mode())
                .physical_address;
        return contains_physical_instruction(physical_instruction);
    } catch (const TranslationError&) {
        return false;
    }
}

bool guest_program_range_contains_instruction(const CpuState& cpu,
                                              const std::uint32_t instruction_address,
                                              const GuestProgramRange range) noexcept {
    return GuestProgramRangeMatcher(range).contains_instruction(cpu, instruction_address);
}

} // namespace katana::runtime
