#include "generated_one_bit_shift_program.cpp"

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
    katana_generated::CpuState primary;

    primary.r[1] = 0x80000001u;
    primary.r[2] = 0x00000003u;
    primary.r[3] = 0x40000001u;
    primary.r[4] = 0x80000003u;

    katana_generated::run(primary);

    require(
        primary.r[1] == 0x00000002u,
        "SHLL besitzt ein falsches Ergebnis."
    );

    require(
        primary.r[2] == 0x00000001u,
        "SHLR besitzt ein falsches Ergebnis."
    );

    require(
        primary.r[3] == 0x80000002u,
        "SHAL besitzt ein falsches Ergebnis."
    );

    require(
        primary.r[4] == 0xC0000001u,
        "SHAR muss das gesetzte Vorzeichenbit erhalten."
    );

    require(
        primary.r[5] == 1u,
        "SHLL muss das alte Bit 31 nach T uebernehmen."
    );

    require(
        primary.r[6] == 1u,
        "SHLR muss das alte Bit 0 nach T uebernehmen."
    );

    require(
        primary.r[7] == 0u,
        "SHAL darf bei altem Bit 31 gleich null T nicht setzen."
    );

    require(
        primary.r[8] == 1u,
        "SHAR muss das alte Bit 0 nach T uebernehmen."
    );

    require(
        primary.t,
        "Das letzte SHAR muss T setzen."
    );

    require(
        primary.pc == 0u,
        "RTS muss den initialen PR-Wert in PC uebernehmen."
    );

    katana_generated::CpuState boundaries;

    boundaries.r[1] = 0x7FFFFFFFu;
    boundaries.r[2] = 0x80000000u;
    boundaries.r[3] = 0x80000000u;
    boundaries.r[4] = 0x7FFFFFFEu;

    katana_generated::run(boundaries);

    require(
        boundaries.r[1] == 0xFFFFFFFEu &&
        boundaries.r[5] == 0u,
        "SHLL-Grenzvektor ist falsch."
    );

    require(
        boundaries.r[2] == 0x40000000u &&
        boundaries.r[6] == 0u,
        "SHLR-Grenzvektor ist falsch."
    );

    require(
        boundaries.r[3] == 0x00000000u &&
        boundaries.r[7] == 1u,
        "SHAL-Grenzvektor ist falsch."
    );

    require(
        boundaries.r[4] == 0x3FFFFFFFu &&
        boundaries.r[8] == 0u,
        "SHAR-Grenzvektor ist falsch."
    );

    require(
        !boundaries.t,
        "Das letzte SHAR darf T fuer ein gerades Ausgangsregister nicht setzen."
    );

    std::cout
        << "KR-1201 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("shll r1", primary.r[1]);
    print_hex("shlr r2", primary.r[2]);
    print_hex("shal r3", primary.r[3]);
    print_hex("shar r4", primary.r[4]);

    std::cout
        << "T-Marker primaer: "
        << std::dec
        << primary.r[5]
        << primary.r[6]
        << primary.r[7]
        << primary.r[8]
        << '\n'
        << "T-Marker Grenze: "
        << boundaries.r[5]
        << boundaries.r[6]
        << boundaries.r[7]
        << boundaries.r[8]
        << '\n';

    return EXIT_SUCCESS;
}
