#include "generated_pc_relative_program.cpp"

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
    require(cpu.gbr == 0x13579BDFu && cpu.mach == 0x2468ACE0u && cpu.macl == 0x11223344u && cpu.t &&
                !cpu.s && cpu.q && !cpu.m,
            "PC-relative Operationen haben fremden CPU-Zustand veraendert.");
}

} // namespace

int main() {
    run_normal_case();

    std::cout << "KR-1405 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
