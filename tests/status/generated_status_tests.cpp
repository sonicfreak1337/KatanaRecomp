#include "generated_status_program.cpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

void require(
    const bool condition,
    const std::string& message
) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void print_hex(
    const char* name,
    const std::uint32_t value
) {
    std::cout
        << name
        << " = 0x"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << value
        << '\n';
}

}

int main() {
    katana_generated::CpuState cpu;

    katana_generated::run(cpu);

    require(
        cpu.r[3] == 1u,
        "CMP/EQ Register oder der erste BF-Pfad ist falsch."
    );

    require(
        cpu.r[4] == 7u,
        "Mindestens eine Statusoperation oder ein bedingter Sprung ist falsch."
    );

    require(
        cpu.t,
        "SETT muss das T-Bit am Programmende gesetzt haben."
    );

    require(
        cpu.pc == 0u,
        "RTS muss den initialen PR-Wert in PC uebernehmen."
    );

    require(
        cpu.r[0] == 5u,
        "Der CMP/EQ-Immediate-Test verlor den R0-Wert."
    );

    std::cout
        << "Generierte T-Bit- und Vergleichssemantik wurde erfolgreich ausgefuehrt.\n";

    std::cout
        << "r3 = "
        << std::dec
        << cpu.r[3]
        << '\n'
        << "r4 = "
        << cpu.r[4]
        << '\n'
        << "t  = "
        << (cpu.t ? 1 : 0)
        << '\n';

    print_hex("pc", cpu.pc);

    return EXIT_SUCCESS;
}
