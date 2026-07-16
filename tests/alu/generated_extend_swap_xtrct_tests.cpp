#include "generated_extend_swap_xtrct_program.cpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void print_hex(const char* name, const std::uint32_t value) {
    std::cout << name << " = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
              << value << '\n';
}

} // namespace

int main() {
    katana_generated::CpuState cpu;

    cpu.r[1] = 0x1234FF80u;
    cpu.r[8] = 0xA1B2C3D4u;
    cpu.r[9] = 0x11223344u;
    cpu.t = true;

    katana_generated::run(cpu);

    require(cpu.r[1] == 0x1234FF80u,
            "Extend und Swap duerfen das Quellregister r1 nicht veraendern.");

    require(cpu.r[2] == 0x00000080u, "EXTU.B besitzt ein falsches Ergebnis.");

    require(cpu.r[3] == 0x0000FF80u, "EXTU.W besitzt ein falsches Ergebnis.");

    require(cpu.r[4] == 0xFFFFFF80u, "EXTS.B besitzt ein falsches Ergebnis.");

    require(cpu.r[5] == 0xFFFFFF80u, "EXTS.W besitzt ein falsches Ergebnis.");

    require(cpu.r[6] == 0x123480FFu, "SWAP.B besitzt ein falsches Ergebnis.");

    require(cpu.r[7] == 0xFF801234u, "SWAP.W besitzt ein falsches Ergebnis.");

    require(cpu.r[8] == 0xA1B2C3D4u, "XTRCT darf das Quellregister r8 nicht veraendern.");

    require(cpu.r[9] == 0xC3D41122u, "XTRCT muss die mittleren 32 Bit aus Rm:Rn extrahieren.");

    require(cpu.t, "KR-1105-Instruktionen duerfen das T-Bit nicht veraendern.");

    require(cpu.pc == 0u, "RTS muss den initialen PR-Wert in PC uebernehmen.");

    std::cout << "KR-1105 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("extu.b r2", cpu.r[2]);
    print_hex("extu.w r3", cpu.r[3]);
    print_hex("exts.b r4", cpu.r[4]);
    print_hex("exts.w r5", cpu.r[5]);
    print_hex("swap.b r6", cpu.r[6]);
    print_hex("swap.w r7", cpu.r[7]);
    print_hex("xtrct r9", cpu.r[9]);

    return EXIT_SUCCESS;
}
