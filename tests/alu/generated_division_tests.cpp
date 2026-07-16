#include "generated_division_program.cpp"

#include <array>
#include <cstdint>
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

struct Div1Case {
    const char* name;
    std::uint32_t rn;
    std::uint32_t rm;
    bool q;
    bool m;
    bool t;
    std::uint32_t expected_rn;
    bool expected_q;
    bool expected_t;
};

void print_case(const Div1Case& test, const katana_generated::CpuState& cpu) {
    std::cout << test.name << ": R6=0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill('0') << cpu.r[6] << " Q=" << std::dec << static_cast<unsigned>(cpu.q)
              << " M=" << static_cast<unsigned>(cpu.m) << " T=" << static_cast<unsigned>(cpu.t)
              << '\n';
}

void run_div0u_case() {
    katana_generated::CpuState cpu;

    cpu.r[3] = 0xAAAAAAAAu;
    cpu.r[4] = 0x55555555u;
    cpu.q = true;
    cpu.m = true;
    cpu.t = true;
    cpu.s = true;
    cpu.pr = 0u;
    cpu.pc = 0x8C010010u;

    katana_generated::fn_8C010010(cpu);

    require(!cpu.q && !cpu.m && !cpu.t, "DIV0U muss Q, M und T loeschen.");

    require(cpu.s, "DIV0U darf S nicht veraendern.");

    require(cpu.r[3] == 0xAAAAAAAAu && cpu.r[4] == 0x55555555u,
            "DIV0U darf allgemeine Register nicht veraendern.");
}

void run_div0s_cases() {
    struct Case {
        const char* name;
        std::uint32_t rn;
        std::uint32_t rm;
        bool expected_q;
        bool expected_m;
        bool expected_t;
    };

    constexpr std::array<Case, 3> cases = {
        {{"positiv durch negativ", 0x80000000u, 0x00000001u, true, false, true},
         {"negativ durch negativ", 0xFFFFFFFFu, 0x80000000u, true, true, false},
         {"negativer divisor", 0x00000001u, 0xFFFFFFFFu, false, true, true}}};

    for (const auto& test : cases) {
        katana_generated::CpuState cpu;

        cpu.r[3] = test.rm;
        cpu.r[4] = test.rn;
        cpu.q = !test.expected_q;
        cpu.m = !test.expected_m;
        cpu.t = !test.expected_t;
        cpu.s = true;
        cpu.pr = 0u;
        cpu.pc = 0x8C010018u;

        katana_generated::fn_8C010018(cpu);

        require(cpu.q == test.expected_q && cpu.m == test.expected_m && cpu.t == test.expected_t,
                std::string(test.name) + ": DIV0S-Flags sind falsch.");

        require(cpu.r[3] == test.rm && cpu.r[4] == test.rn,
                std::string(test.name) + ": DIV0S hat Register veraendert.");

        require(cpu.s, std::string(test.name) + ": DIV0S hat S veraendert.");
    }
}

void run_div1_cases() {
    constexpr std::array<Div1Case, 8> cases = {{{"oldQ0 M0 ohne Borrow",
                                                 0x00000004u,
                                                 0x00000003u,
                                                 false,
                                                 false,
                                                 false,
                                                 0x00000005u,
                                                 false,
                                                 true},
                                                {"oldQ0 M0 Vorzeichenbit",
                                                 0x80000000u,
                                                 0x00000001u,
                                                 false,
                                                 false,
                                                 true,
                                                 0x00000000u,
                                                 true,
                                                 false},
                                                {"oldQ0 M1 mit Carry",
                                                 0x7FFFFFFFu,
                                                 0x00000002u,
                                                 false,
                                                 true,
                                                 false,
                                                 0x00000000u,
                                                 false,
                                                 false},
                                                {"oldQ0 M1 Q gesetzt",
                                                 0xFFFFFFFFu,
                                                 0x00000002u,
                                                 false,
                                                 true,
                                                 true,
                                                 0x00000001u,
                                                 true,
                                                 true},
                                                {"oldQ1 M0 mit Carry",
                                                 0x00000001u,
                                                 0xFFFFFFFFu,
                                                 true,
                                                 false,
                                                 false,
                                                 0x00000001u,
                                                 true,
                                                 false},
                                                {"oldQ1 M0 Vorzeichenbit",
                                                 0x80000000u,
                                                 0xFFFFFFFFu,
                                                 true,
                                                 false,
                                                 true,
                                                 0x00000000u,
                                                 false,
                                                 true},
                                                {"oldQ1 M1 ohne Borrow",
                                                 0x00000004u,
                                                 0x00000003u,
                                                 true,
                                                 true,
                                                 false,
                                                 0x00000005u,
                                                 true,
                                                 true},
                                                {"oldQ1 M1 Vorzeichenbit",
                                                 0x80000000u,
                                                 0x00000001u,
                                                 true,
                                                 true,
                                                 true,
                                                 0x00000000u,
                                                 false,
                                                 false}}};

    for (const auto& test : cases) {
        katana_generated::CpuState cpu;

        cpu.r[5] = test.rm;
        cpu.r[6] = test.rn;
        cpu.q = test.q;
        cpu.m = test.m;
        cpu.t = test.t;
        cpu.s = true;
        cpu.mach = 0x12345678u;
        cpu.macl = 0x9ABCDEF0u;
        cpu.pr = 0u;
        cpu.pc = 0x8C010020u;

        katana_generated::fn_8C010020(cpu);

        require(cpu.r[6] == test.expected_rn && cpu.q == test.expected_q && cpu.m == test.m &&
                    cpu.t == test.expected_t,
                std::string(test.name) + ": DIV1-Ergebnis ist falsch.");

        require(cpu.r[5] == test.rm, std::string(test.name) + ": DIV1 hat den Divisor veraendert.");

        require(cpu.s && cpu.mach == 0x12345678u && cpu.macl == 0x9ABCDEF0u,
                std::string(test.name) + ": DIV1 hat fremden CPU-Zustand veraendert.");

        print_case(test, cpu);
    }
}

void run_complete_chain() {
    katana_generated::CpuState cpu;

    cpu.r[3] = 0x00000003u;
    cpu.r[4] = 0x80000004u;
    cpu.r[5] = 0x00000003u;
    cpu.r[6] = 0x00000004u;
    cpu.q = true;
    cpu.m = true;
    cpu.t = false;
    cpu.s = true;
    cpu.pr = 0u;

    katana_generated::run(cpu);

    require(cpu.r[6] == 0x0000000Cu,
            "Der komplette Divisions-Aufrufspfad besitzt ein falsches Rn.");

    require(!cpu.q && !cpu.m && cpu.t,
            "Der komplette Divisions-Aufrufspfad besitzt falsche Q-/M-/T-Bits.");

    require(cpu.r[3] == 0x00000003u && cpu.r[4] == 0x80000004u && cpu.r[5] == 0x00000003u,
            "Der komplette Divisions-Aufrufspfad hat Quellregister veraendert.");

    require(cpu.s, "Der komplette Divisions-Aufrufspfad hat S veraendert.");
}

} // namespace

int main() {
    run_div0u_case();
    run_div0s_cases();
    run_div1_cases();
    run_complete_chain();

    std::cout << "KR-1304 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
