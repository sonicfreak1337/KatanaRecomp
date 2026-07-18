#include "generated_alpha_integer_program.cpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    katana_generated::CpuState cpu;
    cpu.gbr = 0x100u;
    cpu.r[0] = 4u;
    cpu.r[1] = 0x200u;
    cpu.mach = 0xFFFFFFFFu;
    cpu.macl = 0xFFFFFFFFu;
    cpu.memory.write_u8(0x104u, 0xF0u);
    cpu.memory.write_u8(0x200u, 0x00u);

    katana_generated::run(cpu);

    require(cpu.mach == 0u && cpu.macl == 0u, "CLRMAC loescht nicht beide Akkumulatoren.");
    require(cpu.r[2] == 1u, "TST.B schreibt das Ergebnis nicht ueber MOVT in den Gastzustand.");
    require(cpu.memory.read_u8(0x104u) == 0x0Fu,
            "GBR-Byte-AND/OR/XOR besitzt ein falsches Endergebnis.");
    require(cpu.memory.read_u8(0x200u) == 0x80u && cpu.t,
            "TAS.B setzt Byte oder T-Bit bei einem Nullwert falsch.");
    require(cpu.pc == 0u, "Generierter Alpha-Integer-Test kehrt nicht ueber PR zurueck.");

    cpu.memory.write_u8(0x200u, 0x01u);
    cpu.t = true;
    katana_generated::run(cpu);
    require(cpu.memory.read_u8(0x200u) == 0x81u && !cpu.t,
            "TAS.B behandelt einen gesetzten Bytewert oder das T-Bit falsch.");

    katana_generated::CpuState invalid;
    invalid.gbr = 0x100u;
    invalid.r[0] = 4u;
    invalid.r[1] = 0x00100000u;
    invalid.memory.write_u8(0x104u, 0xF0u);
    katana_generated::run(invalid);
    require(invalid.trap_pending &&
                invalid.last_exception_cause == katana::runtime::ExceptionCause::AddressErrorRead &&
                invalid.tea == 0x00100000u,
            "TAS.B-Fehlerpfad meldet keinen strukturierten Speicherfehler.");

    std::cout << "KR-4502 Alpha-Integer-/Bitsemantik erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
