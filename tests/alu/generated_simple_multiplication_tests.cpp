#include "generated_simple_multiplication_program.cpp"

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

void run_mul_l_case() {
    katana_generated::CpuState cpu;

    cpu.r[1] = 0xFFFFFFFFu;
    cpu.r[2] = 0x00000002u;
    cpu.macl = 0xAAAAAAAAu;
    cpu.t = true;
    cpu.pr = 0u;
    cpu.pc = 0x8C010010u;

    katana_generated::fn_8C010010(cpu);

    require(
        cpu.macl == 0xFFFFFFFEu,
        "MUL.L muss die unteren 32 Produktbits nach MACL schreiben."
    );

    require(
        cpu.r[1] == 0xFFFFFFFFu &&
        cpu.r[2] == 0x00000002u,
        "MUL.L darf die Quellregister nicht veraendern."
    );

    require(
        cpu.t,
        "MUL.L darf T nicht veraendern."
    );

    print_hex("mul.l macl", cpu.macl);
}

void run_muls_w_cases() {
    {
        katana_generated::CpuState cpu;

        cpu.r[3] = 0xABCDFFFEu;
        cpu.r[4] = 0x12348000u;
        cpu.macl = 0xAAAAAAAAu;
        cpu.t = false;
        cpu.pr = 0u;
        cpu.pc = 0x8C010018u;

        katana_generated::fn_8C010018(cpu);

        require(
            cpu.macl == 0x00010000u,
            "MULS.W -2 mal -32768 ist falsch."
        );

        require(
            cpu.r[3] == 0xABCDFFFEu &&
            cpu.r[4] == 0x12348000u,
            "MULS.W darf die Quellregister nicht veraendern."
        );

        require(
            !cpu.t,
            "MULS.W darf T nicht veraendern."
        );

        print_hex("muls.w positiv", cpu.macl);
    }

    {
        katana_generated::CpuState cpu;

        cpu.r[3] = 0xFFFF7FFFu;
        cpu.r[4] = 0xAAAAFFFFu;
        cpu.t = true;
        cpu.pr = 0u;
        cpu.pc = 0x8C010018u;

        katana_generated::fn_8C010018(cpu);

        require(
            cpu.macl == 0xFFFF8001u,
            "MULS.W 32767 mal -1 ist falsch."
        );

        require(
            cpu.t,
            "MULS.W darf ein gesetztes T nicht veraendern."
        );

        print_hex("muls.w negativ", cpu.macl);
    }
}

void run_mulu_w_case() {
    katana_generated::CpuState cpu;

    cpu.r[5] = 0x1234FFFFu;
    cpu.r[6] = 0xABCDFFFFu;
    cpu.macl = 0xAAAAAAAAu;
    cpu.t = false;
    cpu.pr = 0u;
    cpu.pc = 0x8C010020u;

    katana_generated::fn_8C010020(cpu);

    require(
        cpu.macl == 0xFFFE0001u,
        "MULU.W 65535 mal 65535 ist falsch."
    );

    require(
        cpu.r[5] == 0x1234FFFFu &&
        cpu.r[6] == 0xABCDFFFFu,
        "MULU.W darf die Quellregister nicht veraendern."
    );

    require(
        !cpu.t,
        "MULU.W darf T nicht veraendern."
    );

    print_hex("mulu.w macl", cpu.macl);
}

}

int main() {
    run_mul_l_case();
    run_muls_w_cases();
    run_mulu_w_case();

    katana_generated::CpuState chain;

    chain.r[1] = 3u;
    chain.r[2] = 7u;
    chain.r[3] = 0x0000FFFEu;
    chain.r[4] = 0x00000004u;
    chain.r[5] = 0x0000FFFFu;
    chain.r[6] = 0x00000002u;
    chain.pr = 0u;

    katana_generated::run(chain);

    require(
        chain.macl == 0x0001FFFEu,
        "Der komplette Aufrufspfad muss mit dem MULU.W-Ergebnis enden."
    );

    std::cout
        << "KR-1301 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
