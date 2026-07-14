#include "generated_fpu_program.cpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    using katana::runtime::read_dr_double;

    {
        katana_generated::CpuState cpu;
        cpu.write_sr(katana::runtime::sr_fd_mask);
        cpu.vbr = 0x8000u;
        cpu.fr[0] = 0x3F800000u;
        cpu.fr[1] = 0x40000000u;
        cpu.pc = 0x100u;
        katana_generated::fn_00000100(cpu);
        require(
            cpu.last_exception_cause ==
                katana::runtime::ExceptionCause::FpuDisabled &&
            cpu.expevt == katana::runtime::event_fpu_disabled &&
            cpu.spc == 0x100u && cpu.pc == 0x8100u &&
            cpu.fr[1] == 0x40000000u,
            "SR.FD sperrt einen generierten FPU-Pfad nicht vor Zustandsaenderungen."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.write_sr(katana::runtime::sr_fd_mask);
        cpu.memory.set_alignment_policy(katana::runtime::MemoryAlignmentPolicy::Strict);
        cpu.r[4] = 1u;
        cpu.pc = 0x12Cu;
        katana_generated::fn_0000012C(cpu);
        require(
            cpu.last_exception_cause ==
                katana::runtime::ExceptionCause::FpuDisabled &&
            cpu.expevt == katana::runtime::event_fpu_disabled &&
            cpu.spc == 0x12Cu && cpu.tea == 0u,
            "SR.FD wird bei einem FPU-Speicheroperand nicht vor dem Adressfehler geprueft."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.write_sr(katana::runtime::sr_fd_mask);
        cpu.vbr = 0x9000u;
        cpu.fr[0] = 0x3F800000u;
        cpu.fr[1] = 0x40000000u;
        cpu.pc = 0x13Eu;
        katana_generated::fn_0000013E(cpu);
        require(
            cpu.last_exception_cause ==
                katana::runtime::ExceptionCause::SlotFpuDisabled &&
            cpu.expevt == katana::runtime::event_slot_fpu_disabled &&
            cpu.spc == 0x13Eu && cpu.pc == 0x9100u &&
            cpu.exception_in_delay_slot && cpu.fr[1] == 0x40000000u,
            "FPU-Sperre im generierten BRA-Delay-Slot verliert Owner-PC oder Zustand."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.fpul = 0xA5A5A5A5u;
        cpu.fr[3] = 0x3FC00000u;
        cpu.pc = 0x100u;
        katana_generated::fn_00000100(cpu);
        require(
            std::bit_cast<float>(cpu.fr[1]) == 2.0f &&
            cpu.fr[2] == 0xA5A5A5A5u &&
            cpu.fpul == 0x3FC00000u,
            "Generierte Single-Precision-Arithmetik oder FPUL-Transfers sind falsch."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.fr[0] = 0x11111111u;
        cpu.xf[0] = 0x22222222u;
        cpu.pc = 0x110u;
        katana_generated::fn_00000110(cpu);
        require(
            cpu.fpu_register_bank_selected() && cpu.fpu_transfer_pair() &&
            cpu.fr[0] == 0x22222222u && cpu.xf[0] == 0x11111111u,
            "Generiertes FRCHG oder FSCHG ist falsch."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.write_fpscr(katana::runtime::fpscr_pr_mask);
        cpu.fr[0] = 0x11111111u;
        cpu.xf[0] = 0x22222222u;
        cpu.pc = 0x110u;
        katana_generated::fn_00000110(cpu);
        require(
            cpu.last_exception_cause ==
                katana::runtime::ExceptionCause::IllegalInstruction &&
            cpu.expevt == katana::runtime::event_illegal_instruction &&
            cpu.spc == 0x110u && !cpu.fpu_register_bank_selected() &&
            cpu.fr[0] == 0x11111111u && cpu.xf[0] == 0x22222222u,
            "FRCHG wird im Double-Precision-Modus nicht vor Bankaenderungen abgewiesen."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.write_fpscr(katana::runtime::fpscr_pr_mask);
        cpu.fr[2] = 0xAAAAAAAAu;
        cpu.fr[3] = 0xBBBBBBBBu;
        cpu.pc = 0x146u;
        katana_generated::fn_00000146(cpu);
        require(
            cpu.last_exception_cause ==
                katana::runtime::ExceptionCause::IllegalInstruction &&
            cpu.expevt == katana::runtime::event_illegal_instruction &&
            cpu.spc == 0x146u && cpu.fr[2] == 0xAAAAAAAAu &&
            cpu.fr[3] == 0xBBBBBBBBu,
            "Ungerade Double-Precision-Register werden nicht vor Teilwirkungen abgewiesen."
        );
    }

    for (const std::uint32_t reserved_rm : {2u, 3u}) {
        katana_generated::CpuState cpu;
        cpu.write_fpscr(reserved_rm);
        cpu.fr[1] = 0x3F800000u;
        cpu.fr[3] = 0x40000000u;
        cpu.pc = 0x146u;
        katana_generated::fn_00000146(cpu);
        require(
            cpu.last_exception_cause ==
                katana::runtime::ExceptionCause::IllegalInstruction &&
            cpu.expevt == katana::runtime::event_illegal_instruction &&
            cpu.spc == 0x146u && cpu.fr[3] == 0x40000000u,
            "Ein reservierter FPSCR.RM-Wert erreicht die FPU-Ausfuehrung."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.fr[0] = std::bit_cast<std::uint32_t>(1.0f);
        cpu.fr[1] = std::bit_cast<std::uint32_t>(2.0f);
        cpu.fpul = 42u;
        cpu.pc = 0x118u;
        katana_generated::fn_00000118(cpu);
        require(cpu.t && cpu.fpul == 42u, "Generiertes FCMP/FLOAT/FTRC ist falsch.");
    }

    {
        katana_generated::CpuState cpu;
        cpu.write_fpscr(katana::runtime::fpscr_pr_mask);
        katana::runtime::write_dr_double(cpu, 0u, 1.5);
        katana::runtime::write_dr_double(cpu, 2u, 2.25);
        cpu.pc = 0x122u;
        katana_generated::fn_00000122(cpu);
        require(
            read_dr_double(cpu, 2u) == 3.75 &&
            read_dr_double(cpu, 4u) == 3.75,
            "Generierte Double-Precision-Arithmetik oder FCNV-Konvertierung ist falsch."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.memory.set_alignment_policy(katana::runtime::MemoryAlignmentPolicy::Strict);
        cpu.r[0] = 4u;
        cpu.r[4] = 0x40u;
        cpu.r[6] = 0x80u;
        cpu.memory.write_u32(0x40u, 0x40400000u);
        cpu.memory.write_u32(0x48u, 0x40400000u);
        cpu.fr[7] = 0x40800000u;
        cpu.pc = 0x12Cu;
        katana_generated::fn_0000012C(cpu);
        require(
            cpu.fr[5] == 0x40400000u && cpu.fr[8] == 0x40400000u &&
            cpu.fr[9] == 0x40400000u && cpu.r[4] == 0x44u &&
            cpu.r[6] == 0x7Cu && cpu.memory.read_u32(0x7Cu) == 0x40400000u &&
            cpu.memory.read_u32(0x80u) == 0x40400000u,
            "Generierte FMOV-Register- oder Speicherformen sind falsch."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.memory = katana::runtime::Memory(0u);
        cpu.memory.map_region(
            "left",
            0x1000u,
            std::make_shared<katana::runtime::LinearMemoryDevice>(16u)
        );
        cpu.memory.map_region(
            "right",
            0x1010u,
            std::make_shared<katana::runtime::LinearMemoryDevice>(16u)
        );
        cpu.write_fpscr(katana::runtime::fpscr_sz_mask);
        cpu.memory.write_u32(0x100Cu, 0x89ABCDEFu);
        cpu.memory.write_u32(0x1010u, 0x01234567u);
        cpu.r[4] = 0x100Cu;
        cpu.pc = 0x14Cu;
        katana_generated::fn_0000014C(cpu);
        require(
            katana::runtime::read_fpu_pair_bits(cpu, 4u) ==
                0x0123456789ABCDEFull &&
            cpu.r[4] == 0x1014u,
            "64-Bit-FMOV @R4+,FR4 scheitert an einer angrenzenden Regionsgrenze."
        );

        katana::runtime::write_fpu_pair_bits(cpu, 4u, 0xFEDCBA9876543210ull);
        cpu.r[4] = 0x1014u;
        cpu.pc = 0x152u;
        katana_generated::fn_00000152(cpu);
        require(
            cpu.r[4] == 0x100Cu &&
            cpu.memory.read_u32(0x100Cu) == 0x76543210u &&
            cpu.memory.read_u32(0x1010u) == 0xFEDCBA98u,
            "64-Bit-FMOV FR4,@-R4 scheitert bei identischem Registerindex."
        );

        cpu.r[4] = 0x101Cu;
        cpu.fr[4] = 0xAAAAAAAAu;
        cpu.fr[5] = 0xBBBBBBBBu;
        cpu.pc = 0x14Cu;
        katana_generated::fn_0000014C(cpu);
        require(
            cpu.last_exception_cause ==
                katana::runtime::ExceptionCause::BusErrorRead &&
            cpu.spc == 0x14Cu && cpu.r_bank[4] == 0x101Cu &&
            cpu.fr[4] == 0xAAAAAAAAu && cpu.fr[5] == 0xBBBBBBBBu,
            "Fehlgeschlagenes 64-Bit-FMOV an der Speichergrenze hinterlaesst Teilzustand."
        );
    }

    std::cout << "Generierter FPU-Pfad erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
