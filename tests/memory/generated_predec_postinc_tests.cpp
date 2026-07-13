#include "generated_predec_postinc_program.cpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
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

void prepare_cpu(katana_generated::CpuState& cpu) {
    cpu.r[1] = 0xA1B2C3D4u;
    cpu.r[2] = 0x00000101u;

    cpu.r[3] = 0x1122A1B2u;
    cpu.r[4] = 0x00000122u;

    cpu.r[5] = 0x89ABCDEFu;
    cpu.r[6] = 0x00000144u;

    cpu.r[7] = 0x00000160u;
    cpu.memory.write_u8(0x00000160u, 0x80u);

    cpu.r[9] = 0x00000170u;
    cpu.memory.write_u16(0x00000170u, 0x8001u);

    cpu.r[11] = 0x00000180u;
    cpu.memory.write_u32(0x00000180u, 0x76543210u);

    cpu.r[13] = 0x00000210u;

    cpu.r[14] = 0x00000300u;
    cpu.memory.write_u8(0x00000300u, 0x10u);
    cpu.memory.write_u16(0x00000010u, 0x0020u);
    cpu.memory.write_u32(0x00000020u, 0x12345678u);

    cpu.mach = 0x13579BDFu;
    cpu.macl = 0x2468ACE0u;
    cpu.t = true;
    cpu.s = true;
    cpu.q = true;
    cpu.m = false;
    cpu.pr = 0u;
}

void verify_cpu(
    const katana_generated::CpuState& cpu,
    const std::string& context
) {
    require(
        cpu.r[1] == 0xA1B2C3D4u &&
        cpu.r[2] == 0x00000100u &&
        cpu.memory.read_u8(0x00000100u) == 0xD4u,
        context + ": MOV.B Pre-Decrement ist falsch."
    );

    require(
        cpu.r[3] == 0x1122A1B2u &&
        cpu.r[4] == 0x00000120u &&
        cpu.memory.read_u16(0x00000120u) == 0xA1B2u,
        context + ": MOV.W Pre-Decrement ist falsch."
    );

    require(
        cpu.r[5] == 0x89ABCDEFu &&
        cpu.r[6] == 0x00000140u &&
        cpu.memory.read_u32(0x00000140u) == 0x89ABCDEFu,
        context + ": MOV.L Pre-Decrement ist falsch."
    );

    require(
        cpu.r[7] == 0x00000161u &&
        cpu.r[8] == 0xFFFFFF80u,
        context + ": MOV.B Post-Increment ist falsch."
    );

    require(
        cpu.r[9] == 0x00000172u &&
        cpu.r[10] == 0xFFFF8001u,
        context + ": MOV.W Post-Increment ist falsch."
    );

    require(
        cpu.r[11] == 0x00000184u &&
        cpu.r[12] == 0x76543210u,
        context + ": MOV.L Post-Increment ist falsch."
    );

    require(
        cpu.r[13] == 0x00000209u &&
        cpu.memory.read_u8(0x0000020Fu) == 0x10u &&
        cpu.memory.read_u16(0x0000020Du) == 0x020Fu &&
        cpu.memory.read_u32(0x00000209u) == 0x0000020Du,
        context + ": Identische Pre-Decrement-Register sind falsch."
    );

    require(
        cpu.r[14] == 0x12345678u,
        context + ": Identische Post-Increment-Register sind falsch."
    );

    require(
        cpu.mach == 0x13579BDFu &&
        cpu.macl == 0x2468ACE0u &&
        cpu.t &&
        cpu.s &&
        cpu.q &&
        !cpu.m,
        context + ": Die Speicheroperationen haben fremden CPU-Zustand veraendert."
    );
}

void run_direct_functions() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu);

    cpu.pc = 0x8C010030u;
    katana_generated::fn_8C010030(cpu);

    cpu.pc = 0x8C010038u;
    katana_generated::fn_8C010038(cpu);

    cpu.pc = 0x8C010040u;
    katana_generated::fn_8C010040(cpu);

    cpu.pc = 0x8C010048u;
    katana_generated::fn_8C010048(cpu);

    cpu.pc = 0x8C010050u;
    katana_generated::fn_8C010050(cpu);

    cpu.pc = 0x8C010058u;
    katana_generated::fn_8C010058(cpu);

    cpu.pc = 0x8C010060u;
    katana_generated::fn_8C010060(cpu);

    cpu.pc = 0x8C010070u;
    katana_generated::fn_8C010070(cpu);

    verify_cpu(cpu, "Direkte Funktionsaufrufe");
}

void run_complete_chain() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu);

    katana_generated::run(cpu);

    verify_cpu(cpu, "Kompletter BSR-Aufrufspfad");

    print_hex("predec byte adresse", cpu.r[2]);
    print_hex("predec word adresse", cpu.r[4]);
    print_hex("predec long adresse", cpu.r[6]);
    print_hex("postinc byte wert", cpu.r[8]);
    print_hex("postinc word wert", cpu.r[10]);
    print_hex("postinc long wert", cpu.r[12]);
    print_hex("gleiches predec register", cpu.r[13]);
    print_hex("gleiches postinc register", cpu.r[14]);
}

void run_invalid_access_cases() {
    {
        katana_generated::CpuState cpu;
        cpu.r[1] = 0xA1B2C3D4u;
        cpu.r[2] = 0u;
        cpu.pc = 0x8C010030u;

        bool threw = false;
        try {
            katana_generated::fn_8C010030(cpu);
        } catch (const std::out_of_range&) {
            threw = true;
        }

        require(
            threw,
            "Pre-Decrement-Wraparound muss als ungueltige Adresse fehlschlagen."
        );
        require(
            cpu.r[1] == 0xA1B2C3D4u && cpu.r[2] == 0u,
            "Fehlgeschlagenes Pre-Decrement hat Register vorzeitig veraendert."
        );
    }

    {
        katana_generated::CpuState cpu;
        cpu.r[7] = 1024u * 1024u;
        cpu.r[8] = 0xCAFEBABEu;
        cpu.pc = 0x8C010048u;

        bool threw = false;
        try {
            katana_generated::fn_8C010048(cpu);
        } catch (const std::out_of_range&) {
            threw = true;
        }

        require(
            threw,
            "Post-Increment ausserhalb des Runtime-Speichers muss fehlschlagen."
        );
        require(
            cpu.r[7] == 1024u * 1024u &&
            cpu.r[8] == 0xCAFEBABEu,
            "Fehlgeschlagenes Post-Increment hat Register vorzeitig veraendert."
        );
    }
}

}

int main() {
    run_direct_functions();
    run_complete_chain();
    run_invalid_access_cases();

    std::cout
        << "KR-1401 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
