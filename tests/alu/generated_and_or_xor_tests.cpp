#include "generated_and_or_xor_program.cpp"

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
        cpu.r[1] == 0u,
        "AND Register muss 0xFFFFFFF0 & 0x0F zu 0 reduzieren."
    );

    require(
        cpu.r[2] == 15u,
        "Logikoperationen duerfen das Quellregister nicht veraendern."
    );

    require(
        cpu.r[3] == 0xFFFFFFFFu,
        "OR Register besitzt ein falsches Ergebnis."
    );

    require(
        cpu.r[4] == 0xFFFFFFFFu,
        "XOR Register besitzt ein falsches Ergebnis."
    );

    require(
        cpu.r[0] == 0x70u,
        "Immediate-AND, OR und XOR besitzen ein falsches Endergebnis."
    );

    require(
        cpu.pc == 0u,
        "RTS muss den initialen PR-Wert in PC uebernehmen."
    );

    std::cout
        << "KR-1102 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("r0", cpu.r[0]);
    print_hex("r1", cpu.r[1]);
    print_hex("r2", cpu.r[2]);
    print_hex("r3", cpu.r[3]);
    print_hex("r4", cpu.r[4]);

    return EXIT_SUCCESS;
}
