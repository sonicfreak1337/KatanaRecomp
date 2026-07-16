#include "generated_carry_overflow_program.cpp"

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

void require_markers(const katana_generated::CpuState& cpu, const std::uint32_t expected) {
    require(cpu.r[7] == expected, "ADDC-T-Marker ist falsch.");
    require(cpu.r[8] == expected, "SUBC-T-Marker ist falsch.");
    require(cpu.r[9] == expected, "NEGC-T-Marker ist falsch.");
    require(cpu.r[10] == expected, "ADDV-T-Marker ist falsch.");
    require(cpu.r[15] == expected, "SUBV-T-Marker ist falsch.");
}

void print_hex(const char* name, const std::uint32_t value) {
    std::cout << name << " = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
              << value << '\n';
}

} // namespace

int main() {
    katana_generated::CpuState overflow_cpu;

    overflow_cpu.r[1] = 0xFFFFFFFFu;
    overflow_cpu.r[2] = 0u;

    overflow_cpu.r[3] = 0u;
    overflow_cpu.r[4] = 0u;

    overflow_cpu.r[5] = 1u;

    overflow_cpu.r[11] = 0x7FFFFFFFu;
    overflow_cpu.r[12] = 1u;

    overflow_cpu.r[13] = 0x80000000u;
    overflow_cpu.r[14] = 1u;

    katana_generated::run(overflow_cpu);

    require(overflow_cpu.r[1] == 0u, "ADDC muss 0xFFFFFFFF + 0 + T zu 0 mit Carry berechnen.");

    require(overflow_cpu.r[2] == 0u, "ADDC darf das Quellregister nicht veraendern.");

    require(overflow_cpu.r[3] == 0xFFFFFFFFu, "SUBC muss 0 - 0 - T mit Borrow berechnen.");

    require(overflow_cpu.r[6] == 0xFFFFFFFFu, "NEGC muss 0 - 1 - 0 berechnen.");

    require(overflow_cpu.r[11] == 0x80000000u, "ADDV-Grenzwert ist falsch.");

    require(overflow_cpu.r[13] == 0x7FFFFFFFu, "SUBV-Grenzwert ist falsch.");

    require_markers(overflow_cpu, 1u);

    require(overflow_cpu.t, "SUBV muss beim signed Overflow T setzen.");

    require(overflow_cpu.pc == 0u, "RTS muss den initialen PR-Wert in PC uebernehmen.");

    katana_generated::CpuState normal_cpu;

    normal_cpu.r[1] = 1u;
    normal_cpu.r[2] = 1u;

    normal_cpu.r[3] = 5u;
    normal_cpu.r[4] = 1u;

    normal_cpu.r[5] = 0u;

    normal_cpu.r[11] = 1u;
    normal_cpu.r[12] = 1u;

    normal_cpu.r[13] = 5u;
    normal_cpu.r[14] = 1u;

    katana_generated::run(normal_cpu);

    require(normal_cpu.r[1] == 3u, "ADDC ohne Carry-Out muss 1 + 1 + T zu 3 berechnen.");

    require(normal_cpu.r[3] == 3u, "SUBC ohne Borrow-Out muss 5 - 1 - T zu 3 berechnen.");

    require(normal_cpu.r[6] == 0u, "NEGC von 0 ohne Borrow-In muss 0 ergeben.");

    require(normal_cpu.r[11] == 2u, "ADDV ohne Overflow muss 1 + 1 zu 2 berechnen.");

    require(normal_cpu.r[13] == 4u, "SUBV ohne Overflow muss 5 - 1 zu 4 berechnen.");

    require_markers(normal_cpu, 0u);

    require(!normal_cpu.t, "SUBV ohne signed Overflow muss T loeschen.");

    std::cout << "KR-1104 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("carry r1", overflow_cpu.r[1]);
    print_hex("borrow r3", overflow_cpu.r[3]);
    print_hex("negc r6", overflow_cpu.r[6]);
    print_hex("addv r11", overflow_cpu.r[11]);
    print_hex("subv r13", overflow_cpu.r[13]);

    std::cout << "Overflow-Marker = " << std::dec << overflow_cpu.r[7] << overflow_cpu.r[8]
              << overflow_cpu.r[9] << overflow_cpu.r[10] << overflow_cpu.r[15] << '\n'
              << "Normal-Marker   = " << normal_cpu.r[7] << normal_cpu.r[8] << normal_cpu.r[9]
              << normal_cpu.r[10] << normal_cpu.r[15] << '\n';

    return EXIT_SUCCESS;
}
