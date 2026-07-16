#include "generated_system_register_program.cpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using TransferFunction = void (*)(katana_generated::CpuState&);

void run_direct_cases() {
    constexpr std::array functions = {katana_generated::fn_00000100,
                                      katana_generated::fn_00000110,
                                      katana_generated::fn_00000120,
                                      katana_generated::fn_00000130,
                                      katana_generated::fn_00000140,
                                      katana_generated::fn_00000150,
                                      katana_generated::fn_00000160,
                                      katana_generated::fn_00000170,
                                      katana_generated::fn_00000180,
                                      katana_generated::fn_00000190,
                                      katana_generated::fn_000001A0,
                                      katana_generated::fn_000001B0,
                                      katana_generated::fn_000001C0,
                                      katana_generated::fn_000001D0,
                                      katana_generated::fn_000001E0,
                                      katana_generated::fn_000001F0,
                                      katana_generated::fn_00000200,
                                      katana_generated::fn_00000210,
                                      katana_generated::fn_00000220};

    for (std::size_t index = 0; index < functions.size(); ++index) {
        katana_generated::CpuState cpu;
        if (index >= 5u && index != 6u) cpu.write_sr(0x40000000u);
        cpu.r[0] = 0xFFFFFFFFu;
        cpu.pc = 0x100u + static_cast<std::uint32_t>(index) * 0x10u;
        functions[index](cpu);
        const auto expected = index == 4u ? 0x003FFFFFu : index == 5u ? 0x700083F3u : 0xFFFFFFFFu;
        require(cpu.r[1] == expected, "Direkter Spezialregistertransfer ist falsch.");
    }

    katana_generated::CpuState cpu;
    cpu.write_sr(0x40000000u);
    cpu.sgr = 0x89ABCDEFu;
    cpu.pc = 0x230u;
    katana_generated::fn_00000230(cpu);
    require(cpu.r[1] == 0x89ABCDEFu, "STC SGR,Rn ist falsch.");
}

void run_sr_bank_case() {
    katana_generated::CpuState cpu;
    cpu.write_sr(0x40000000u);
    cpu.r[0] = 0x60000001u;
    cpu.r[2] = 0x11111111u;
    cpu.r_bank[2] = 0x22222222u;
    cpu.pc = 0x150u;
    katana_generated::fn_00000150(cpu);

    require(cpu.r[2] == 0x22222222u && cpu.r_bank[2] == 0x11111111u,
            "LDC SR wechselt die aktive Registerbank nicht.");
    require(cpu.t && cpu.read_sr() == 0x60000001u, "SR-Bits wurden falsch synchronisiert.");
}

void reject_user_mode_control_register_access() {
    katana_generated::CpuState cpu;
    cpu.vbr = 0x8000u;
    cpu.pc = 0x170u;
    cpu.write_sr(0u);
    cpu.r[0] = 0x12345678u;
    katana_generated::fn_00000170(cpu);
    require(cpu.trap_pending &&
                cpu.last_exception_cause == katana::runtime::ExceptionCause::IllegalInstruction &&
                cpu.spc == 0x170u && cpu.pc == 0x8100u && cpu.vbr == 0x8000u,
            "Privilegierter Systemregisterzugriff wird im User-Modus nicht abgelehnt.");
}

void run_memory_cases() {
    katana_generated::CpuState cpu;
    cpu.pr = 0x89ABCDEFu;
    cpu.r[2] = 0x204u;
    cpu.pc = 0x240u;
    katana_generated::fn_00000240(cpu);
    require(cpu.memory.read_u32(0x200u) == 0x89ABCDEFu && cpu.mach == 0x89ABCDEFu &&
                cpu.r[2] == 0x204u,
            "STS.L/LDS.L oder Little Endian sind falsch.");

    cpu.write_sr(0x40000303u);
    cpu.r[8] = 0x304u;
    cpu.pc = 0x250u;
    katana_generated::fn_00000250(cpu);
    require(cpu.memory.read_u32(0x300u) == 0x40000303u && cpu.read_sr() == 0x40000303u &&
                cpu.r[8] == 0x304u,
            "SR-Speicherformen sind falsch.");
}

void run_invalid_cases() {
    {
        katana_generated::CpuState cpu;
        cpu.pr = 0x12345678u;
        cpu.r[2] = 0u;
        cpu.pc = 0x260u;
        katana_generated::fn_00000260(cpu);
        require(cpu.trap_pending &&
                    cpu.last_exception_cause == katana::runtime::ExceptionCause::BusErrorWrite &&
                    cpu.tea == 0xFFFFFFFCu && cpu.spc == 0x00000260u,
                "Fehlgeschlagenes STS.L erzeugt keine Bus-Exception.");
        katana::runtime::return_from_exception(cpu);
        require(cpu.r[2] == 0u, "Fehlgeschlagenes STS.L aendert Rn.");
    }
    {
        katana_generated::CpuState cpu;
        cpu.pr = 0x12345678u;
        cpu.r[2] = 1024u * 1024u - 2u;
        cpu.pc = 0x270u;
        katana_generated::fn_00000270(cpu);
        require(cpu.trap_pending &&
                    cpu.last_exception_cause == katana::runtime::ExceptionCause::BusErrorRead &&
                    cpu.tea == 1024u * 1024u - 2u && cpu.spc == 0x00000270u,
                "Fehlgeschlagenes LDS.L erzeugt keine Bus-Exception.");
        katana::runtime::return_from_exception(cpu);
        require(cpu.r[2] == 1024u * 1024u - 2u && cpu.pr == 0x12345678u,
                "Fehlgeschlagenes LDS.L aendert Registerzustand.");
    }
}

} // namespace

int main() {
    run_direct_cases();
    run_sr_bank_case();
    reject_user_mode_control_register_access();
    run_memory_cases();
    run_invalid_cases();
    std::cout << "KR-1406/KR-4503 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
