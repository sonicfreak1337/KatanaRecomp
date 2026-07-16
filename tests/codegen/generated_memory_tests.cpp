#include "generated_memory_program.cpp"

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

    require(cpu.r[1] == 0x40u, "r1 muss die Testadresse 0x40 enthalten.");

    require(cpu.r[3] == 0xFFFFFF80u, "MOV.B-Load muss das Byte mit Vorzeichen erweitern.");

    require(cpu.r[5] == 0xFFFFFFFFu, "MOV.W-Load muss das Word mit Vorzeichen erweitern.");

    require(cpu.r[6] == 127u, "MOV.L-Load muss den gespeicherten 32-Bit-Wert laden.");

    require(cpu.memory.read_u8(0x40u) == 0x7Fu,
            "Das niederwertige Byte des Long-Stores ist falsch.");

    require(cpu.memory.read_u8(0x41u) == 0x00u && cpu.memory.read_u8(0x42u) == 0x00u &&
                cpu.memory.read_u8(0x43u) == 0x00u,
            "Der Long-Store ist nicht korrekt Little Endian.");

    require(cpu.pc == 0u, "Der finale PC muss dem initialen PR-Wert entsprechen.");

    std::cout << "Generierte Speicherzugriffe wurden erfolgreich ausgefuehrt.\n";

    print_hex("r3", cpu.r[3]);
    print_hex("r5", cpu.r[5]);

    std::cout << "r6 = " << std::dec << cpu.r[6] << '\n';

    std::cout << "memory[0x40..0x43] = " << std::hex << std::uppercase << std::setw(2)
              << std::setfill('0') << static_cast<unsigned>(cpu.memory.read_u8(0x40u)) << " "
              << std::setw(2) << static_cast<unsigned>(cpu.memory.read_u8(0x41u)) << " "
              << std::setw(2) << static_cast<unsigned>(cpu.memory.read_u8(0x42u)) << " "
              << std::setw(2) << static_cast<unsigned>(cpu.memory.read_u8(0x43u)) << '\n';

    return EXIT_SUCCESS;
}
