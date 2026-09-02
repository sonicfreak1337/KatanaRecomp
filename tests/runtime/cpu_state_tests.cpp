#include "katana/runtime/aot_runtime_abi.hpp"
#include "katana/build_contract.hpp"
#include "katana/runtime/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <std::size_t Size> bool all_zero(const std::array<std::uint32_t, Size>& values) {
    return std::all_of(
        values.begin(), values.end(), [](const std::uint32_t value) { return value == 0u; });
}

} // namespace

int main() {
    using katana::runtime::CpuState;

    static_assert(katana::runtime::general_register_count == 16u);
    static_assert(katana::runtime::banked_register_count == 8u);
    static_assert(katana::runtime::fpu_register_count == 16u);

    static_assert(katana::runtime::abi_version ==
                  katana::build_contract::aot_runtime_abi_version);

    CpuState cpu;

    require(cpu.memory.alignment_policy() == katana::runtime::MemoryAlignmentPolicy::Permissive,
            "Der historische CpuState-Kompatibilitaetsspeicher muss permissiv bleiben.");

    require(cpu.r.size() == katana::runtime::general_register_count &&
                cpu.r_bank.size() == katana::runtime::banked_register_count &&
                cpu.fr.size() == katana::runtime::fpu_register_count &&
                cpu.xf.size() == katana::runtime::fpu_register_count,
            "Eine zentrale Registerbank besitzt die falsche Groesse.");

    require(all_zero(cpu.r) && all_zero(cpu.r_bank) && all_zero(cpu.fr) && all_zero(cpu.xf),
            "Die zentralen Registerbaenke sind nicht deterministisch nullinitialisiert.");

    constexpr std::array<std::uint32_t, 4u> bank_states{0u,
                                                        katana::runtime::sr_rb_mask,
                                                        katana::runtime::sr_md_mask,
                                                        katana::runtime::sr_md_mask |
                                                            katana::runtime::sr_rb_mask};
    for (const auto initial : bank_states) {
        for (const auto target : bank_states) {
            CpuState matrix;
            for (std::size_t index = 0u; index < matrix.r_bank.size(); ++index) {
                matrix.r[index] = 0x10000000u + static_cast<std::uint32_t>(index);
                matrix.r_bank[index] = 0x20000000u + static_cast<std::uint32_t>(index);
            }
            matrix.write_sr(initial);
            matrix.write_sr(target);
            const bool selected = (target & katana::runtime::sr_md_mask) != 0u &&
                                  (target & katana::runtime::sr_rb_mask) != 0u;
            bool identities_match = true;
            for (std::size_t index = 0u; index < matrix.r_bank.size(); ++index) {
                const auto bank0 = 0x10000000u + static_cast<std::uint32_t>(index);
                const auto bank1 = 0x20000000u + static_cast<std::uint32_t>(index);
                identities_match = identities_match &&
                                   matrix.r[index] == (selected ? bank1 : bank0) &&
                                   matrix.r_bank[index] == (selected ? bank0 : bank1);
            }
            require(identities_match && matrix.register_bank_selected() == selected &&
                        (matrix.read_sr() &
                         (katana::runtime::sr_md_mask | katana::runtime::sr_rb_mask)) == target,
                    "Vier-Zustaende-Matrix vertauscht User/Privileged-RB-Registeridentitaeten.");
        }
    }

    require(cpu.pc == 0u && cpu.pr == 0u && cpu.gbr == 0u && cpu.vbr == 0u && cpu.ssr == 0u &&
                cpu.spc == 0u && cpu.sgr == 0u && cpu.dbr == 0u && cpu.tra == 0u && cpu.tea == 0u &&
                cpu.expevt == 0u && cpu.intevt == 0u && cpu.mach == 0u && cpu.macl == 0u &&
                cpu.fpul == 0u && cpu.fpscr == 0u && cpu.read_sr() == 0u,
            "Ein zentrales SH-4-Register besitzt keinen definierten Grundwert.");

    require(!cpu.t && !cpu.s && !cpu.q && !cpu.m && !cpu.trap_pending &&
                cpu.last_exception_cause == katana::runtime::ExceptionCause::None &&
                !cpu.exception_in_delay_slot && !cpu.sleeping && cpu.last_prefetch_address == 0u &&
                cpu.prefetch_count == 0u && cpu.retired_guest_instructions == 0u &&
                !cpu.last_prefetch_was_store_queue,
            "Ein Runtime-Zustandsflag besitzt keinen definierten Grundwert.");

    cpu.fr[0] = 0x3F800000u;
    cpu.fr[15] = 0x7FC00001u;
    cpu.xf[0] = 0xBF800000u;
    cpu.xf[15] = 0x80000000u;

    require(cpu.fr[0] == 0x3F800000u && cpu.fr[15] == 0x7FC00001u && cpu.xf[0] == 0xBF800000u &&
                cpu.xf[15] == 0x80000000u,
            "FR- oder XF-Rohbits werden nicht unveraendert gespeichert.");

    require(cpu.fr[0] != cpu.xf[0] && cpu.fr[15] != cpu.xf[15],
            "FR- und XF-Bank sind nicht voneinander getrennt.");

    cpu.write_fpscr(katana::runtime::fpscr_fr_mask | katana::runtime::fpscr_pr_mask |
                    katana::runtime::fpscr_sz_mask | katana::runtime::fpscr_dn_mask | 0xFFC00000u);
    require(cpu.fpu_register_bank_selected() && cpu.fpu_double_precision() &&
                cpu.fpu_transfer_pair() && cpu.fpu_flush_denormals() && cpu.fr[0] == 0xBF800000u &&
                cpu.xf[0] == 0x3F800000u && cpu.read_fpscr() == 0x003C0000u,
            "FPSCR-Modi oder FR-/XF-Bankumschaltung sind nicht zentral modelliert.");

    cpu.toggle_fpu_register_bank();
    require(!cpu.fpu_register_bank_selected() && cpu.fr[0] == 0x3F800000u &&
                cpu.xf[0] == 0xBF800000u,
            "FRCHG-Grundsemantik stellt die sichtbaren FPU-Baenke nicht wieder her.");

    cpu.intevt = 0x00000320u;
    require(cpu.intevt == 0x00000320u && cpu.expevt == 0u && cpu.tra == 0u,
            "INTEVT ist nicht als eigenstaendiges Ereignisregister modelliert.");

    cpu.r[0] = 0x11111111u;
    cpu.r_bank[0] = 0x22222222u;
    require(cpu.r[0] == 0x11111111u && cpu.r_bank[0] == 0x22222222u,
            "Allgemeine und banked Register sind nicht getrennt.");

    cpu.write_sr(katana::runtime::sr_md_mask | katana::runtime::sr_rb_mask |
                 katana::runtime::sr_bl_mask | katana::runtime::sr_fd_mask);
    cpu.set_interrupt_mask(9u);
    require(cpu.interrupt_mask() == 9u && cpu.interrupts_blocked() && cpu.privileged_mode() &&
                cpu.register_bank_selected() && cpu.fpu_disabled(),
            "Relevante SR-Felder werden nicht strukturiert abgebildet.");

    cpu.set_interrupt_mask(0xFFu);
    require(cpu.interrupt_mask() == 15u, "Die Interruptmaske wird nicht auf vier Bit begrenzt.");

    constexpr auto scalar_mask =
        katana::runtime::native_aot_scalar_register_bit(
            katana::runtime::NativeAotScalarRegister::T) |
        katana::runtime::native_aot_scalar_register_bit(
            katana::runtime::NativeAotScalarRegister::Pr) |
        katana::runtime::native_aot_scalar_register_bit(
            katana::runtime::NativeAotScalarRegister::Gbr) |
        katana::runtime::native_aot_scalar_register_bit(
            katana::runtime::NativeAotScalarRegister::Mach) |
        katana::runtime::native_aot_scalar_register_bit(
            katana::runtime::NativeAotScalarRegister::Macl) |
        katana::runtime::native_aot_scalar_register_bit(
            katana::runtime::NativeAotScalarRegister::Fpul);
    using LocalRegisters =
        katana::runtime::NativeAotRegisterFile<(1u << 0u) | (1u << 8u), scalar_mask>;

    CpuState released_cpu;
    released_cpu.r[0] = 0x100u;
    released_cpu.r[8] = 0x108u;
    released_cpu.r_bank[0] = 0x200u;
    released_cpu.t = false;
    released_cpu.pr = 0x300u;
    released_cpu.gbr = 0x400u;
    released_cpu.mach = 0x500u;
    released_cpu.macl = 0x600u;
    released_cpu.fpul = 0x700u;
    {
        LocalRegisters locals(released_cpu);
        require(locals.owns_registers() && locals[0] == 0x100u && locals[8] == 0x108u &&
                    !locals.t() && locals.pr() == 0x300u && locals.gbr() == 0x400u &&
                    locals.mach() == 0x500u && locals.macl() == 0x600u &&
                    locals.fpul() == 0x700u,
                "Native-AOT-Registerdatei erwirbt GPR-/Spezialzustand nicht atomar.");
        locals[0] = 0x101u;
        locals[8] = 0x109u;
        locals.t() = true;
        locals.pr() = 0x301u;
        locals.gbr() = 0x401u;
        locals.mach() = 0x501u;
        locals.macl() = 0x601u;
        locals.fpul() = 0x701u;
        locals.flush_release();
        require(!locals.owns_registers() && released_cpu.r[0] == 0x101u &&
                    released_cpu.r[8] == 0x109u && released_cpu.t &&
                    released_cpu.pr == 0x301u && released_cpu.gbr == 0x401u &&
                    released_cpu.mach == 0x501u && released_cpu.macl == 0x601u &&
                    released_cpu.fpul == 0x701u,
                "flush_release veroeffentlicht nicht alle ausgewaehlten Register.");

        released_cpu.write_sr(katana::runtime::sr_md_mask | katana::runtime::sr_rb_mask);
        released_cpu.t = false;
        released_cpu.pr = 0x310u;
        released_cpu.gbr = 0x410u;
        released_cpu.mach = 0x510u;
        released_cpu.macl = 0x610u;
        released_cpu.fpul = 0x710u;
        locals[0] = 0xDEAD0000u;
        locals.pr() = 0xDEAD0001u;
    }
    require(released_cpu.r[0] == 0x200u && released_cpu.r[8] == 0x109u &&
                !released_cpu.t && released_cpu.pr == 0x310u &&
                released_cpu.gbr == 0x410u && released_cpu.mach == 0x510u &&
                released_cpu.macl == 0x610u && released_cpu.fpul == 0x710u,
            "Freigegebener Destructor schreibt stale GPR-/Spezialwerte in neuen Zustand.");

    CpuState reacquired_cpu;
    reacquired_cpu.r[0] = 0x110u;
    reacquired_cpu.r_bank[0] = 0x220u;
    reacquired_cpu.pr = 0x330u;
    {
        LocalRegisters locals(reacquired_cpu);
        locals.flush_release();
        reacquired_cpu.write_sr(katana::runtime::sr_md_mask | katana::runtime::sr_rb_mask);
        reacquired_cpu.pr = 0x331u;
        locals.reload_acquire();
        require(locals.owns_registers() && locals[0] == 0x220u && locals.pr() == 0x331u,
                "reload_acquire beobachtet neue Registerbank/Spezialwerte nicht.");
        locals[0] = 0x221u;
        locals.pr() = 0x332u;
    }
    require(reacquired_cpu.r[0] == 0x221u && reacquired_cpu.pr == 0x332u,
            "Erneut erworbene Register werden am Native-AOT-Ausgang nicht veroeffentlicht.");

    require(cpu.memory.size() == 1024u * 1024u,
            "Der vollstaendige CPU-Zustand verlor seinen Runtime-Speicher.");

    std::cout << "Vollstaendiger zentraler CPU-Zustand erfolgreich.\n";
    return EXIT_SUCCESS;
}
