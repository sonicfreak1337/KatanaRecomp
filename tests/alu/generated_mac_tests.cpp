#include "generated_mac_program.cpp"

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

void print_mac(const char* name, const katana_generated::CpuState& cpu) {
    std::cout << name << " = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
              << cpu.mach << ':' << std::setw(8) << std::setfill('0') << cpu.macl << '\n';
}

void run_macw_normal() {
    katana_generated::CpuState cpu;

    cpu.r[1] = 0x00000100u;
    cpu.r[2] = 0x00000110u;
    cpu.memory.write_u16(0x00000100u, 0x0003u);
    cpu.memory.write_u16(0x00000110u, 0xFFFCu);
    cpu.mach = 0u;
    cpu.macl = 1u;
    cpu.t = true;
    cpu.s = false;
    cpu.pr = 0u;
    cpu.pc = 0x8C010018u;

    katana_generated::fn_8C010018(cpu);

    require(cpu.mach == 0xFFFFFFFFu && cpu.macl == 0xFFFFFFF5u,
            "MAC.W ohne Saettigung ist falsch.");

    require(cpu.r[1] == 0x00000102u && cpu.r[2] == 0x00000112u,
            "MAC.W hat die Adressregister falsch fortgeschaltet.");

    require(cpu.t && !cpu.s, "MAC.W hat Statusbits veraendert.");
    print_mac("mac.w normal", cpu);
}

void run_macl_normal() {
    katana_generated::CpuState cpu;

    cpu.r[3] = 0x00000120u;
    cpu.r[4] = 0x00000130u;
    cpu.memory.write_u32(0x00000120u, 0xFFFFFFFEu);
    cpu.memory.write_u32(0x00000130u, 0x00000007u);
    cpu.mach = 0u;
    cpu.macl = 10u;
    cpu.t = false;
    cpu.s = false;
    cpu.pr = 0u;
    cpu.pc = 0x8C010020u;

    katana_generated::fn_8C010020(cpu);

    require(cpu.mach == 0xFFFFFFFFu && cpu.macl == 0xFFFFFFFCu,
            "MAC.L ohne Saettigung ist falsch.");

    require(cpu.r[3] == 0x00000124u && cpu.r[4] == 0x00000134u,
            "MAC.L hat die Adressregister falsch fortgeschaltet.");

    require(!cpu.t && !cpu.s, "MAC.L hat Statusbits veraendert.");
    print_mac("mac.l normal", cpu);
}

void run_same_register_cases() {
    {
        katana_generated::CpuState cpu;

        cpu.r[5] = 0x00000140u;
        cpu.memory.write_u16(0x00000140u, 4u);
        cpu.memory.write_u16(0x00000142u, 5u);
        cpu.pr = 0u;
        cpu.pc = 0x8C010028u;

        katana_generated::fn_8C010028(cpu);

        require(cpu.mach == 0u && cpu.macl == 20u && cpu.r[5] == 0x00000144u,
                "MAC.W mit identischem Register ist falsch.");

        print_mac("mac.w gleiches register", cpu);
    }

    {
        katana_generated::CpuState cpu;

        cpu.r[6] = 0x00000150u;
        cpu.memory.write_u32(0x00000150u, 0xFFFFFFFDu);
        cpu.memory.write_u32(0x00000154u, 7u);
        cpu.pr = 0u;
        cpu.pc = 0x8C010030u;

        katana_generated::fn_8C010030(cpu);

        require(cpu.mach == 0xFFFFFFFFu && cpu.macl == 0xFFFFFFEBu && cpu.r[6] == 0x00000158u,
                "MAC.L mit identischem Register ist falsch.");

        print_mac("mac.l gleiches register", cpu);
    }
}

void run_macw_saturation() {
    {
        katana_generated::CpuState cpu;

        cpu.r[1] = 0x00000180u;
        cpu.r[2] = 0x00000190u;
        cpu.memory.write_u16(0x00000180u, 100u);
        cpu.memory.write_u16(0x00000190u, 100u);
        cpu.mach = 0x12345678u;
        cpu.macl = 0x7FFFFFF0u;
        cpu.s = true;
        cpu.pr = 0u;
        cpu.pc = 0x8C010018u;

        katana_generated::fn_8C010018(cpu);

        require(cpu.mach == 0x12345678u && cpu.macl == 0x7FFFFFFFu,
                "Positive MAC.W-Saettigung ist falsch.");
    }

    {
        katana_generated::CpuState cpu;

        cpu.r[1] = 0x000001A0u;
        cpu.r[2] = 0x000001B0u;
        cpu.memory.write_u16(0x000001A0u, 0xFF9Cu);
        cpu.memory.write_u16(0x000001B0u, 100u);
        cpu.mach = 0x89ABCDEFu;
        cpu.macl = 0x80000010u;
        cpu.s = true;
        cpu.pr = 0u;
        cpu.pc = 0x8C010018u;

        katana_generated::fn_8C010018(cpu);

        require(cpu.mach == 0x89ABCDEFu && cpu.macl == 0x80000000u,
                "Negative MAC.W-Saettigung ist falsch.");
    }
}

void run_macl_saturation() {
    {
        katana_generated::CpuState cpu;

        cpu.r[3] = 0x000001C0u;
        cpu.r[4] = 0x000001D0u;
        cpu.memory.write_u32(0x000001C0u, 100u);
        cpu.memory.write_u32(0x000001D0u, 1u);
        cpu.mach = 0x00007FFFu;
        cpu.macl = 0xFFFFFFF0u;
        cpu.s = true;
        cpu.pr = 0u;
        cpu.pc = 0x8C010020u;

        katana_generated::fn_8C010020(cpu);

        require(cpu.mach == 0x00007FFFu && cpu.macl == 0xFFFFFFFFu,
                "Positive MAC.L-Saettigung ist falsch.");
    }

    {
        katana_generated::CpuState cpu;

        cpu.r[3] = 0x000001E0u;
        cpu.r[4] = 0x000001F0u;
        cpu.memory.write_u32(0x000001E0u, 0xFFFFFF9Cu);
        cpu.memory.write_u32(0x000001F0u, 1u);
        cpu.mach = 0x00008000u;
        cpu.macl = 0x00000010u;
        cpu.s = true;
        cpu.pr = 0u;
        cpu.pc = 0x8C010020u;

        katana_generated::fn_8C010020(cpu);

        require(cpu.mach == 0x00008000u && cpu.macl == 0x00000000u,
                "Negative MAC.L-Saettigung ist falsch.");
    }

    {
        katana_generated::CpuState cpu;

        cpu.r[3] = 0x00000200u;
        cpu.r[4] = 0x00000210u;
        cpu.memory.write_u32(0x00000200u, 0xFFFFFFFEu);
        cpu.memory.write_u32(0x00000210u, 3u);
        cpu.s = true;
        cpu.pr = 0u;
        cpu.pc = 0x8C010020u;

        katana_generated::fn_8C010020(cpu);

        require(cpu.mach == 0x0000FFFFu && cpu.macl == 0xFFFFFFFAu,
                "Negative MAC.L-48-Bit-Darstellung ist falsch.");
    }
}

void run_complete_chain() {
    katana_generated::CpuState cpu;

    cpu.r[1] = 0x00000220u;
    cpu.r[2] = 0x00000230u;
    cpu.r[3] = 0x00000240u;
    cpu.r[4] = 0x00000250u;
    cpu.r[5] = 0x00000260u;
    cpu.r[6] = 0x00000270u;

    cpu.memory.write_u16(0x00000220u, 2u);
    cpu.memory.write_u16(0x00000230u, 3u);
    cpu.memory.write_u32(0x00000240u, 4u);
    cpu.memory.write_u32(0x00000250u, 5u);
    cpu.memory.write_u16(0x00000260u, 6u);
    cpu.memory.write_u16(0x00000262u, 7u);
    cpu.memory.write_u32(0x00000270u, 8u);
    cpu.memory.write_u32(0x00000274u, 9u);

    cpu.s = true;
    cpu.pr = 0u;

    katana_generated::run(cpu);

    require(!cpu.s, "SETS und CLRS muessen den Einstieg im geloeschten S-Modus verlassen.");

    require(cpu.mach == 0u && cpu.macl == 140u,
            "Der komplette MAC-Aufrufspfad besitzt ein falsches Ergebnis.");

    require(cpu.r[1] == 0x00000222u && cpu.r[2] == 0x00000232u && cpu.r[3] == 0x00000244u &&
                cpu.r[4] == 0x00000254u && cpu.r[5] == 0x00000264u && cpu.r[6] == 0x00000278u,
            "Der komplette MAC-Aufrufspfad hat Register falsch fortgeschaltet.");
}

} // namespace

int main() {
    run_macw_normal();
    run_macl_normal();
    run_same_register_cases();
    run_macw_saturation();
    run_macl_saturation();
    run_complete_chain();

    std::cout << "KR-1303 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
