#include "generated_execution_program.cpp"

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

void print_hex(const char* name, const std::uint32_t value) {
    std::cout << name << " = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
              << value << '\n';
}

} // namespace

int main() {
    katana_generated::CpuState cpu;

    katana_generated::run(cpu);

    require(cpu.r[1] == 4u, "r1 muss nach MOV #5 und ADD #-1 den Wert 4 besitzen.");

    require(cpu.r[2] == 7u, "Der Delay Slot des BSR muss r2 auf 7 setzen.");

    require(cpu.pc == 0x8C010004u,
            "Der finale PC entspricht nicht der SH-4-PR-Semantik des Testprogramms.");

    require(cpu.pr == 0x8C010004u, "PR muss die Rueckkehradresse des BSR enthalten.");

    std::cout << "Generierter C++-Code wurde erfolgreich ausgefuehrt.\n";
    std::cout << "r1 = " << std::dec << cpu.r[1] << '\n';
    std::cout << "r2 = " << std::dec << cpu.r[2] << '\n';
    print_hex("pc", cpu.pc);
    print_hex("pr", cpu.pr);

    return EXIT_SUCCESS;
}
