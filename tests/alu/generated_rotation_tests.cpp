#include "generated_rotation_program.cpp"

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
    primary.r[2] = 0x80000001u;
    primary.r[3] = 0x80000000u;
    primary.r[4] = 0x00000002u;
    primary.t = false;

    katana_generated::run(primary);

    require(
        primary.r[1] == 0x00000003u &&
        primary.r[5] == 1u,
        "ROTL-Primaervektor ist falsch."
    );

    require(
        primary.r[2] == 0xC0000000u &&
        primary.r[6] == 1u,
        "ROTR-Primaervektor ist falsch."
    );

    require(
        primary.r[3] == 0x00000001u &&
        primary.r[7] == 1u,
        "ROTCL muss das alte T-Bit in Bit 0 uebernehmen."
    );

    require(
        primary.r[4] == 0x80000001u &&
        primary.r[8] == 0u,
        "ROTCR muss das alte T-Bit in Bit 31 uebernehmen."
    );

    require(
        !primary.t,
        "Das letzte ROTCR muss T aus dem alten Bit 0 loeschen."
    );

    require(
        primary.pc == 0u,
        "RTS muss den initialen PR-Wert in PC uebernehmen."
    );

    katana_generated::CpuState alternate;

    alternate.r[1] = 0x00000002u;
    alternate.r[2] = 0x00000002u;
    alternate.r[3] = 0x00000001u;
    alternate.r[4] = 0x00000001u;
    alternate.t = false;

    katana_generated::run(alternate);

    require(
        alternate.r[1] == 0x00000004u &&
        alternate.r[5] == 0u,
        "ROTL-Alternativvektor ist falsch."
    );

    require(
        alternate.r[2] == 0x00000001u &&
        alternate.r[6] == 0u,
        "ROTR-Alternativvektor ist falsch."
    );

    require(
        alternate.r[3] == 0x00000002u &&
        alternate.r[7] == 0u,
        "ROTCL mit geloeschtem T-Bit ist falsch."
    );

    require(
        alternate.r[4] == 0x00000000u &&
        alternate.r[8] == 1u,
        "ROTCR mit geloeschtem Eingangs-T ist falsch."
    );

    require(
        alternate.t,
        "Das letzte ROTCR muss T aus dem alten Bit 0 setzen."
    );

    std::cout
        << "KR-1203 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    print_hex("rotl r1", primary.r[1]);
    print_hex("rotr r2", primary.r[2]);
    print_hex("rotcl r3", primary.r[3]);
    print_hex("rotcr r4", primary.r[4]);

    std::cout
        << "T-Marker primaer: "
        << std::dec
        << primary.r[5]
        << primary.r[6]
        << primary.r[7]
        << primary.r[8]
        << '\n'
        << "T-Marker alternativ: "
        << alternate.r[5]
        << alternate.r[6]
        << alternate.r[7]
        << alternate.r[8]
        << '\n';

    return EXIT_SUCCESS;
}
