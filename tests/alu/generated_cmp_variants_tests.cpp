#include "generated_cmp_variants_program.cpp"

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

    for (std::size_t index = 3u; index <= 9u; ++index) {
        require(
            cpu.r[index] == 1u,
            "Mindestens eine CMP-Variante oder ihr Branch-Pfad ist falsch."
        );
    }

    require(
        cpu.r[1] == 0xFFFFFFFFu,
        "CMP darf das Zielregister r1 nicht veraendern."
    );

    require(
        cpu.r[2] == 1u,
        "CMP darf das Quellregister r2 nicht veraendern."
    );

    require(
        cpu.r[10] == 0xFFFFFFFFu,
        "CMP/STR darf das Quellregister r10 nicht veraendern."
    );

    require(
        cpu.t,
        "CMP/STR muss bei gleichen Bytepaaren T setzen."
    );

    require(
        cpu.pc == 0u,
        "RTS muss den initialen PR-Wert in PC uebernehmen."
    );

    std::cout
        << "KR-1103 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("r1", cpu.r[1]);
    print_hex("r2", cpu.r[2]);

    std::cout
        << "Marker r3-r9 =";

    for (std::size_t index = 3u; index <= 9u; ++index) {
        std::cout << " " << std::dec << cpu.r[index];
    }

    std::cout
        << "\nt = "
        << (cpu.t ? 1 : 0)
        << '\n';

    return EXIT_SUCCESS;
}
