#include "generated_pc_relative_program.cpp"

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

void set_preserved_state(katana_generated::CpuState& cpu) {
    cpu.gbr = 0x13579BDFu;
    cpu.mach = 0x2468ACE0u;
    cpu.macl = 0x11223344u;
    cpu.t = true;
    cpu.s = false;
    cpu.q = true;
    cpu.m = false;
}

void run_normal_case() {
    katana_generated::CpuState cpu;
    cpu.memory.write_u16(0x00000108u, 0x8001u);
    cpu.memory.write_u32(0x00000110u, 0x76543210u);
    set_preserved_state(cpu);

    katana_generated::run(cpu);

    require(cpu.r[1] == 0xFFFF8001u, "PC-relativer MOV.W erweitert falsch.");
    require(cpu.r[2] == 0x76543210u, "PC-relativer MOV.L laedt falsch.");
    require(cpu.r[0] == 0x00000118u, "MOVA berechnet die falsche Adresse.");
    require(
        cpu.gbr == 0x13579BDFu &&
        cpu.mach == 0x2468ACE0u &&
        cpu.macl == 0x11223344u &&
        cpu.t && !cpu.s && cpu.q && !cpu.m,
        "PC-relative Operationen haben fremden CPU-Zustand veraendert."
    );
}

void run_invalid_access_case() {
    katana_generated::CpuState cpu;
    cpu.r[3] = 0xA1B2C3D4u;

    bool threw = false;
    try {
        cpu.pc = 0x00000200u;
        katana_generated::fn_00000200(cpu);
    } catch (const std::out_of_range&) {
        threw = true;
    }

    require(threw, "Ein PC-relativer Zugriff ausserhalb des Speichers muss fehlschlagen.");
    require(
        cpu.r[3] == 0xA1B2C3D4u,
        "Ein fehlgeschlagener PC-relativer Load hat das Zielregister veraendert."
    );
}

void run_zero_address_case() {
    katana_generated::CpuState cpu;
    cpu.memory.write_u32(0u, 0x89ABCDEFu);
    cpu.r[4] = 0u;
    cpu.pc = 0x00000210u;

    katana_generated::fn_00000210(cpu);

    require(
        cpu.r[4] == 0x89ABCDEFu,
        "Ein PC-relativer Load von Adresse null wurde falsch ausgefuehrt."
    );
}

}

int main() {
    run_normal_case();
    run_invalid_access_case();
    run_zero_address_case();

    std::cout
        << "KR-1405 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
