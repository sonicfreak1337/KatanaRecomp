#include "generated_dynamic_shift_program.cpp"

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

void run_case(
    const char* name,
    const std::uint32_t logical_count,
    const std::uint32_t logical_value,
    const std::uint32_t expected_logical,
    const std::uint32_t arithmetic_count,
    const std::uint32_t arithmetic_value,
    const std::uint32_t expected_arithmetic,
    const bool initial_t
) {
    katana_generated::CpuState cpu;

    cpu.r[1] = logical_count;
    cpu.r[2] = logical_value;
    cpu.r[3] = arithmetic_count;
    cpu.r[4] = arithmetic_value;
    cpu.t = initial_t;

    katana_generated::run(cpu);

    require(
        cpu.r[2] == expected_logical,
        std::string(name) + ": SHLD-Ergebnis ist falsch."
    );

    require(
        cpu.r[4] == expected_arithmetic,
        std::string(name) + ": SHAD-Ergebnis ist falsch."
    );

    require(
        cpu.r[8] == (initial_t ? 1u : 0u) &&
        cpu.r[9] == (initial_t ? 1u : 0u),
        std::string(name) + ": T-Marker wurden veraendert."
    );

    require(
        cpu.t == initial_t,
        std::string(name) + ": T-Bit wurde veraendert."
    );

    require(
        cpu.r[1] == logical_count &&
        cpu.r[3] == arithmetic_count,
        std::string(name) + ": Quellregister wurden veraendert."
    );

    require(
        cpu.pc == 0u,
        std::string(name) + ": RTS-Ergebnis ist falsch."
    );

    std::cout << name << '\n';
    print_hex("  shld", cpu.r[2]);
    print_hex("  shad", cpu.r[4]);
}

}

int main() {
    run_case(
        "positiv +4",
        4u,
        0x12345678u,
        0x23456780u,
        4u,
        0x81234567u,
        0x12345670u,
        true
    );

    run_case(
        "negativ -4",
        0xFFFFFFFCu,
        0xF2345678u,
        0x0F234567u,
        0xFFFFFFFCu,
        0x81234567u,
        0xF8123456u,
        false
    );

    run_case(
        "grosser Zaehler +36 und -36",
        36u,
        0x12345678u,
        0x23456780u,
        0xFFFFFFDCu,
        0x81234567u,
        0xF8123456u,
        true
    );

    run_case(
        "Grenze -32 mit negativem SHAD-Wert",
        0xFFFFFFE0u,
        0x89ABCDEFu,
        0x00000000u,
        0xFFFFFFE0u,
        0x81234567u,
        0xFFFFFFFFu,
        false
    );

    run_case(
        "Grenze -32 mit positivem SHAD-Wert",
        0x80000000u,
        0xFFFFFFFFu,
        0x00000000u,
        0x80000000u,
        0x71234567u,
        0x00000000u,
        true
    );

    std::cout
        << "KR-1204 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
