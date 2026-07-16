#include "generated_sub_neg_not_program.cpp"

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

    katana_generated::run(cpu);

    require(cpu.r[1] == 3u, "SUB muss 5 - 2 = 3 ergeben.");

    require(cpu.r[2] == 2u, "SUB darf das Quellregister nicht veraendern.");

    require(cpu.r[3] == 0xFFFFFFFDu, "NEG muss 0 - 3 mit 32-Bit-Wraparound berechnen.");

    require(cpu.r[4] == 0xFFFFFFFDu, "NOT von 2 ist falsch.");

    require(cpu.r[5] == 0xFFFFFFFEu, "SUB-Wraparound fuer 0xFFFFFFFF - 1 ist falsch.");

    require(cpu.r[6] == 1u, "Neue ALU-Operationen duerfen das Quellregister r6 nicht veraendern.");

    require(cpu.r[7] == 0xFFFFFFFFu, "NEG von 1 ist falsch.");

    require(cpu.r[8] == 0xFFFFFFFEu, "NOT von 1 ist falsch.");

    require(cpu.pc == 0u, "RTS muss den initialen PR-Wert in PC uebernehmen.");

    std::cout << "KR-1101 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    std::cout << "r1 = " << std::dec << cpu.r[1] << '\n';

    print_hex("r3", cpu.r[3]);
    print_hex("r4", cpu.r[4]);
    print_hex("r5", cpu.r[5]);
    print_hex("r7", cpu.r[7]);
    print_hex("r8", cpu.r[8]);

    return EXIT_SUCCESS;
}
