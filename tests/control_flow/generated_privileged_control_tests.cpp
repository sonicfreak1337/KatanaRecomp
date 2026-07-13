#include "generated_privileged_control_program.cpp"

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

void run_trapa_and_rte() {
    katana_generated::CpuState cpu;
    cpu.vbr = 0x8000u;
    cpu.r[0] = 0x11111111u;
    cpu.r_bank[0] = 0x22222222u;
    cpu.r[15] = 0x12345678u;
    cpu.write_sr(0x00000303u);

    katana_generated::run(cpu);

    require(
        cpu.tra == 0x000001FCu && cpu.expevt == 0x00000160u,
        "TRAPA schreibt TRA oder EXPEVT falsch."
    );
    require(
        cpu.ssr == 0x00000303u && cpu.spc == 0x00000102u &&
        cpu.sgr == 0x12345678u,
        "TRAPA sichert SR, PC oder R15 falsch."
    );
    require(
        cpu.read_sr() == 0x70000303u && cpu.pc == 0x00008100u &&
        cpu.trap_pending && cpu.r[0] == 0x22222222u,
        "TRAPA aktiviert Modus, Bank oder Vektor falsch."
    );

    cpu.ssr = 0x00000001u;
    cpu.spc = 0xDEADBEEFu;
    cpu.pc = 0x102u;
    katana_generated::fn_00000102(cpu);

    require(
        cpu.read_sr() == 0x00000001u && cpu.pc == 0xDEADBEEFu &&
        !cpu.trap_pending,
        "RTE stellt SR, PC oder Trapzustand falsch wieder her."
    );
    require(
        cpu.r[0] == 0x11111112u && cpu.r_bank[0] == 0x22222222u,
        "Der RTE-Delay-Slot sieht nicht die wiederhergestellte Registerbank."
    );
}

void run_sleep() {
    katana_generated::CpuState cpu;
    cpu.r[3] = 0x89ABCDEFu;
    cpu.write_sr(0x40000001u);
    cpu.pc = 0x106u;

    katana_generated::fn_00000106(cpu);

    require(cpu.sleeping && cpu.pc == 0x108u, "SLEEP haelt nicht am Folge-PC an.");
    require(
        cpu.r[3] == 0x89ABCDEFu && cpu.read_sr() == 0x40000001u,
        "SLEEP erhaelt den CPU-Zustand nicht."
    );
}

}

int main() {
    run_trapa_and_rte();
    run_sleep();
    std::cout << "KR-1407 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
