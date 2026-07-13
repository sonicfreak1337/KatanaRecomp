#include "generated_fixed_shift_program.cpp"

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

void require_markers(
    const katana_generated::CpuState& cpu,
    const std::uint32_t expected
) {
    for (std::size_t index = 7u; index <= 12u; ++index) {
        require(
            cpu.r[index] == expected,
            "Ein MOVT-Marker besitzt einen falschen Wert."
        );
    }
}

}

int main() {
    katana_generated::CpuState primary;

    primary.r[1] = 0xC0000001u;
    primary.r[2] = 0x12345678u;
    primary.r[3] = 0x89ABCDEFu;
    primary.r[4] = 0x80000003u;
    primary.r[5] = 0xFEDCBA98u;
    primary.r[6] = 0x89ABCDEFu;
    primary.t = true;

    katana_generated::run(primary);

    require(
        primary.r[1] == 0x00000004u,
        "SHLL2 besitzt ein falsches Ergebnis."
    );
    require(
        primary.r[2] == 0x34567800u,
        "SHLL8 besitzt ein falsches Ergebnis."
    );
    require(
        primary.r[3] == 0xCDEF0000u,
        "SHLL16 besitzt ein falsches Ergebnis."
    );
    require(
        primary.r[4] == 0x20000000u,
        "SHLR2 besitzt ein falsches Ergebnis."
    );
    require(
        primary.r[5] == 0x00FEDCBAu,
        "SHLR8 besitzt ein falsches Ergebnis."
    );
    require(
        primary.r[6] == 0x000089ABu,
        "SHLR16 besitzt ein falsches Ergebnis."
    );

    require_markers(primary, 1u);

    require(
        primary.t,
        "Feste Mehrfach-Shifts muessen ein gesetztes T-Bit erhalten."
    );

    require(
        primary.pc == 0u,
        "RTS muss den initialen PR-Wert in PC uebernehmen."
    );

    katana_generated::CpuState boundaries;

    boundaries.r[1] = 0xFFFFFFFFu;
    boundaries.r[2] = 0xFFFFFFFFu;
    boundaries.r[3] = 0xFFFFFFFFu;
    boundaries.r[4] = 0xFFFFFFFFu;
    boundaries.r[5] = 0xFFFFFFFFu;
    boundaries.r[6] = 0xFFFFFFFFu;
    boundaries.t = false;

    katana_generated::run(boundaries);

    require(
        boundaries.r[1] == 0xFFFFFFFCu,
        "SHLL2-Grenzvektor ist falsch."
    );
    require(
        boundaries.r[2] == 0xFFFFFF00u,
        "SHLL8-Grenzvektor ist falsch."
    );
    require(
        boundaries.r[3] == 0xFFFF0000u,
        "SHLL16-Grenzvektor ist falsch."
    );
    require(
        boundaries.r[4] == 0x3FFFFFFFu,
        "SHLR2-Grenzvektor ist falsch."
    );
    require(
        boundaries.r[5] == 0x00FFFFFFu,
        "SHLR8-Grenzvektor ist falsch."
    );
    require(
        boundaries.r[6] == 0x0000FFFFu,
        "SHLR16-Grenzvektor ist falsch."
    );

    require_markers(boundaries, 0u);

    require(
        !boundaries.t,
        "Feste Mehrfach-Shifts muessen ein geloeschtes T-Bit erhalten."
    );

    std::cout
        << "KR-1202 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("shll2 r1", primary.r[1]);
    print_hex("shll8 r2", primary.r[2]);
    print_hex("shll16 r3", primary.r[3]);
    print_hex("shlr2 r4", primary.r[4]);
    print_hex("shlr8 r5", primary.r[5]);
    print_hex("shlr16 r6", primary.r[6]);

    std::cout
        << "T-Marker gesetzt: "
        << std::dec
        << primary.r[7]
        << primary.r[8]
        << primary.r[9]
        << primary.r[10]
        << primary.r[11]
        << primary.r[12]
        << '\n'
        << "T-Marker geloescht: "
        << boundaries.r[7]
        << boundaries.r[8]
        << boundaries.r[9]
        << boundaries.r[10]
        << boundaries.r[11]
        << boundaries.r[12]
        << '\n';

    return EXIT_SUCCESS;
}
