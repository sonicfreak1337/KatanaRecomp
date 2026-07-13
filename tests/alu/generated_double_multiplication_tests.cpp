#include "generated_double_multiplication_program.cpp"

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

void print_product(
    const char* name,
    const katana_generated::CpuState& cpu
) {
    std::cout
        << name
        << " = 0x"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << cpu.mach
        << ':'
        << std::setw(8)
        << std::setfill('0')
        << cpu.macl
        << '\n';
}

void run_dmulu_case(
    const char* name,
    const std::uint32_t source,
    const std::uint32_t destination,
    const std::uint32_t expected_mach,
    const std::uint32_t expected_macl,
    const bool initial_t
) {
    katana_generated::CpuState cpu;

    cpu.r[1] = source;
    cpu.r[2] = destination;
    cpu.mach = 0xAAAAAAAAu;
    cpu.macl = 0x55555555u;
    cpu.t = initial_t;
    cpu.pr = 0u;
    cpu.pc = 0x8C010010u;

    katana_generated::fn_8C010010(cpu);

    require(
        cpu.mach == expected_mach &&
        cpu.macl == expected_macl,
        std::string(name) + ": DMULU.L-Ergebnis ist falsch."
    );

    require(
        cpu.r[1] == source &&
        cpu.r[2] == destination,
        std::string(name) + ": DMULU.L hat Quellregister veraendert."
    );

    require(
        cpu.t == initial_t,
        std::string(name) + ": DMULU.L hat T veraendert."
    );

    print_product(name, cpu);
}

void run_dmuls_case(
    const char* name,
    const std::uint32_t source,
    const std::uint32_t destination,
    const std::uint32_t expected_mach,
    const std::uint32_t expected_macl,
    const bool initial_t
) {
    katana_generated::CpuState cpu;

    cpu.r[3] = source;
    cpu.r[4] = destination;
    cpu.mach = 0xAAAAAAAAu;
    cpu.macl = 0x55555555u;
    cpu.t = initial_t;
    cpu.pr = 0u;
    cpu.pc = 0x8C010018u;

    katana_generated::fn_8C010018(cpu);

    require(
        cpu.mach == expected_mach &&
        cpu.macl == expected_macl,
        std::string(name) + ": DMULS.L-Ergebnis ist falsch."
    );

    require(
        cpu.r[3] == source &&
        cpu.r[4] == destination,
        std::string(name) + ": DMULS.L hat Quellregister veraendert."
    );

    require(
        cpu.t == initial_t,
        std::string(name) + ": DMULS.L hat T veraendert."
    );

    print_product(name, cpu);
}

}

int main() {
    run_dmulu_case(
        "dmulu max",
        0xFFFFFFFFu,
        0xFFFFFFFFu,
        0xFFFFFFFEu,
        0x00000001u,
        true
    );

    run_dmulu_case(
        "dmulu bit63",
        0x80000000u,
        0x00000002u,
        0x00000001u,
        0x00000000u,
        false
    );

    run_dmulu_case(
        "dmulu gemischt",
        0x12345678u,
        0x9ABCDEF0u,
        0x0B00EA4Eu,
        0x242D2080u,
        true
    );

    run_dmuls_case(
        "dmuls minus eins",
        0xFFFFFFFFu,
        0x00000002u,
        0xFFFFFFFFu,
        0xFFFFFFFEu,
        false
    );

    run_dmuls_case(
        "dmuls int min mal zwei",
        0x80000000u,
        0x00000002u,
        0xFFFFFFFFu,
        0x00000000u,
        true
    );

    run_dmuls_case(
        "dmuls int min mal minus eins",
        0x80000000u,
        0xFFFFFFFFu,
        0x00000000u,
        0x80000000u,
        false
    );

    run_dmuls_case(
        "dmuls int max quadrat",
        0x7FFFFFFFu,
        0x7FFFFFFFu,
        0x3FFFFFFFu,
        0x00000001u,
        true
    );

    katana_generated::CpuState chain;

    chain.r[1] = 0xFFFFFFFFu;
    chain.r[2] = 0xFFFFFFFFu;
    chain.r[3] = 0xFFFFFFFFu;
    chain.r[4] = 0x00000002u;
    chain.t = true;
    chain.pr = 0u;

    katana_generated::run(chain);

    require(
        chain.mach == 0xFFFFFFFFu &&
        chain.macl == 0xFFFFFFFEu,
        "Der komplette Aufrufspfad muss mit dem DMULS.L-Ergebnis enden."
    );

    require(
        chain.t,
        "Der komplette Aufrufspfad darf T nicht veraendern."
    );

    std::cout
        << "KR-1302 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
