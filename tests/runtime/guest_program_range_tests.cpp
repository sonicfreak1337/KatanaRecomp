#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/guest_program_range.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace katana::runtime;

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        CpuState direct;
        direct.write_sr(sr_md_mask);
        direct.address_space = std::make_shared<RuntimeAddressSpace>();
        constexpr GuestProgramRange boot_range{dreamcast_disc_boot_address, 0x20u};
        const GuestProgramRangeMatcher boot_matcher(boot_range);
        require(valid_guest_program_range(boot_range),
                "Gueltige BootExecutable-Range wird abgelehnt.");
        require(boot_matcher.valid(),
                "Vorbereiteter Matcher verwirft eine gueltige BootExecutable-Range.");
        for (const auto range_alias : {0x0C010000u, 0x8C010000u, 0xAC010000u}) {
            const GuestProgramRange aliased_range{range_alias, boot_range.byte_size};
            require(valid_guest_program_range(aliased_range),
                    "Lineare P0/P1/P2-Bootrange wird abgelehnt.");
            for (const auto instruction_alias :
                 {0x0C010000u, 0x8C010000u, 0xAC010000u})
                require(guest_program_range_contains_instruction(
                            direct, instruction_alias, aliased_range),
                        "P0/P1/P2-Alias erreicht dieselbe BootExecutable-Range nicht.");
        }
        require(boot_matcher.contains_instruction(direct, 0xAC01001Eu) &&
                    boot_matcher.contains_instruction(direct, 0xCC01001Eu) &&
                    !boot_matcher.contains_instruction(direct, 0xAC01001Fu) &&
                    !boot_matcher.contains_instruction(direct, 0xAC010020u) &&
                    !boot_matcher.contains_instruction(direct, 0xAC00FFFEu),
                "BootExecutable-Grenzen enthalten kein vollstaendiges SH-4-Halbwort exakt.");

        require(!boot_matcher.contains_instruction(
                    direct, dreamcast_system_bootstrap_entry_address) &&
                    !boot_matcher.contains_instruction(
                        direct, dreamcast_system_bootstrap_entry_address + 2u),
                "Mehrere IP.BIN-Bloecke ausserhalb von BootExecutable erzeugen einen "
                "Gastprogramm-Nachweis.");

        CpuState no_space_user;
        no_space_user.write_sr(0u);
        require(!boot_matcher.contains_instruction(no_space_user, 0xCC010000u) &&
                    !boot_matcher.contains_instruction(no_space_user, 0xEC010000u),
                "P3-Userzugriff oder P4-Instruktion wird ohne Address-Space als "
                "BootExecutable-Alias akzeptiert.");

        require(!valid_guest_program_range({0x8C010000u, 0u}) &&
                    !valid_guest_program_range({0x8C010000u, 1u}) &&
                    !valid_guest_program_range({0x8C010001u, 2u}) &&
                    !valid_guest_program_range({0x9FFFFFFEu, 4u}) &&
                    !valid_guest_program_range({0xFFFFFFFEu, 4u}) &&
                    !guest_program_range_contains_instruction(
                        direct, 0x8C010000u, {0xFFFFFFFEu, 4u}),
                "Leere, unalignte oder ueberlaufende Bootrange wird akzeptiert.");

        const auto& direct_space = *direct.address_space;
        require(direct_space.instruction_translation_path(0x0C010000u, true) ==
                        InstructionTranslationPath::Direct &&
                    direct_space.instruction_translation_path(0x8C010000u, true) ==
                        InstructionTranslationPath::Direct &&
                    direct_space.instruction_translation_path(0xAC010000u, true) ==
                        InstructionTranslationPath::Direct &&
                    direct_space.instruction_translation_path(0xCC010000u, true) ==
                        InstructionTranslationPath::Direct &&
                    direct_space.instruction_translation_path(0x7C000000u, false) ==
                        InstructionTranslationPath::Direct &&
                    direct_space.instruction_translation_path(0xE0000000u, true) ==
                        InstructionTranslationPath::Invalid &&
                    direct_space.instruction_translation_path(0x8C010000u, false) ==
                        InstructionTranslationPath::Invalid,
                "No-MMU-, P1/P2-, On-Chip- oder ungueltige Instruktionspfade werden nicht "
                "ohne TLB-Lookup klassifiziert.");

        CpuState mmu;
        mmu.write_sr(sr_md_mask);
        mmu.address_space = std::make_shared<RuntimeAddressSpace>();
        mmu.address_space->set_mode(AddressTranslationMode::Mmu);
        mmu.address_space->write_mmucr(1u);
        require(mmu.address_space->instruction_translation_path(0x00123000u, true) ==
                        InstructionTranslationPath::Mapped &&
                    mmu.address_space->instruction_translation_path(0x0C010000u, true) ==
                        InstructionTranslationPath::Mapped &&
                    mmu.address_space->instruction_translation_path(0x8C010000u, true) ==
                        InstructionTranslationPath::Direct &&
                    mmu.address_space->instruction_translation_path(0xAC010000u, true) ==
                        InstructionTranslationPath::Direct &&
                    mmu.address_space->instruction_translation_path(0xCC010000u, true) ==
                        InstructionTranslationPath::Mapped &&
                    mmu.address_space->instruction_translation_path(0x7C000000u, false) ==
                        InstructionTranslationPath::Direct &&
                    mmu.address_space->instruction_translation_path(0xFF000000u, true) ==
                        InstructionTranslationPath::Invalid,
                "Aktive MMU klassifiziert direkte und potentiell gemappte Instruktionssegmente "
                "nicht strukturell getrennt.");
        mmu.address_space->ldtlb(
            {0x00123000u,
             0x0C010000u,
             4096u,
             0u,
             0u,
             true,
             true,
             true,
             true,
             true,
             true,
             false});
        mmu.address_space->ldtlb(
            {0x0C010000u,
             0x0C010000u,
             4096u,
             0u,
             1u,
             true,
             true,
             true,
             true,
             true,
             true,
             false});
        // Even a colliding TLB entry must never intercept P1: the direct segment remains a cheap
        // physical alias and proves that the fast path does not accidentally walk mapped state.
        mmu.address_space->ldtlb(
            {0x8C010000u,
             0x0D000000u,
             4096u,
             0u,
             2u,
             true,
             true,
             true,
             true,
             true,
             true,
             false});
        mmu.address_space->ldtlb(
            {0xCC010000u,
             0x0C010000u,
             4096u,
             0u,
             3u,
             true,
             true,
             true,
             true,
             true,
             true,
             false});
        require(guest_program_range_contains_instruction(
                    mmu, 0x00123000u, boot_range) &&
                    guest_program_range_contains_instruction(
                        mmu, 0x0012301Eu, boot_range) &&
                    guest_program_range_contains_instruction(
                        mmu, 0x0C010000u, boot_range) &&
                    guest_program_range_contains_instruction(
                        mmu, 0x8C010000u, boot_range) &&
                    guest_program_range_contains_instruction(
                        mmu, 0xAC010000u, boot_range) &&
                    !guest_program_range_contains_instruction(
                        mmu, 0x00123020u, boot_range) &&
                    boot_matcher.contains_instruction(mmu, 0x00123000u) &&
                    boot_matcher.contains_instruction(mmu, 0x8C010000u) &&
                    boot_matcher.contains_instruction(mmu, 0xAC010000u) &&
                    boot_matcher.contains_instruction(mmu, 0xCC010000u),
                "Aktive MMU-Instruktionsabbildung wird nicht gegen die physische Bootrange "
                "geprueft.");

        CpuState miss;
        miss.write_sr(sr_md_mask);
        miss.pc = 0x11111110u;
        miss.tea = 0x22222222u;
        miss.spc = 0x33333332u;
        miss.ssr = 0x44444444u;
        miss.expevt = 0x55555554u;
        miss.last_exception_cause = ExceptionCause::Trap;
        miss.trap_pending = true;
        miss.exception_in_delay_slot = true;
        miss.r[0] = 0x66666666u;
        miss.address_space = std::make_shared<RuntimeAddressSpace>();
        miss.address_space->set_mode(AddressTranslationMode::Mmu);
        miss.address_space->write_mmucr(1u);
        const auto address_space_before = miss.address_space->snapshot();
        const auto sr_before = miss.read_sr();
        require(!boot_matcher.contains_instruction(miss, 0x00123000u) &&
                    miss.pc == 0x11111110u && miss.tea == 0x22222222u &&
                    miss.spc == 0x33333332u && miss.ssr == 0x44444444u &&
                    miss.expevt == 0x55555554u &&
                    miss.last_exception_cause == ExceptionCause::Trap &&
                    miss.trap_pending && miss.exception_in_delay_slot &&
                    miss.r[0] == 0x66666666u && miss.read_sr() == sr_before &&
                    miss.address_space->snapshot() == address_space_before,
                "MMU-Translationsfehler mutiert CPU-/Exceptionzustand statt false zu liefern.");

        std::cout << "Gastprogramm-Range-Nachweistests erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
