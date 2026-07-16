#include "generated_dt_movt_program.cpp"

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

    cpu.t = true;

    katana_generated::run(cpu);

    require(cpu.r[1] == 0u, "Zweimal DT auf dem Startwert 2 muss 0 ergeben.");

    require(cpu.r[2] == 0u, "MOVT nach DT von 2 auf 1 muss 0 schreiben.");

    require(cpu.r[3] == 1u, "MOVT nach DT von 1 auf 0 muss 1 schreiben.");

    require(cpu.r[4] == 0xFFFFFFFFu, "DT auf 0 muss mit 32-Bit-Wraparound 0xFFFFFFFF ergeben.");

    require(cpu.r[5] == 0u, "MOVT nach DT von 0 auf 0xFFFFFFFF muss 0 schreiben.");

    require(!cpu.t, "MOVT darf das durch das letzte DT geloeschte T-Bit nicht veraendern.");

    require(cpu.pc == 0u, "RTS muss den initialen PR-Wert in PC uebernehmen.");

    std::cout << "KR-1106 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("r1", cpu.r[1]);
    print_hex("r2", cpu.r[2]);
    print_hex("r3", cpu.r[3]);
    print_hex("r4", cpu.r[4]);
    print_hex("r5", cpu.r[5]);

    std::cout << "t = " << (cpu.t ? 1 : 0) << '\n';

    return EXIT_SUCCESS;
}
