#include "katana/analysis/value_analysis.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
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

}

int main() {
    constexpr std::array<std::uint8_t, 10> bytes{
        0x05u, 0xE1u,
        0x03u, 0x71u,
        0x13u, 0x62u,
        0x09u, 0x00u,
        0x2Cu, 0x32u
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    const auto trace = katana::analysis::propagate_local_constants(lines);
    require(trace.size() == 5u, "Die lokale Spur ist unvollstaendig.");
    require(trace[0].after.registers[1] == 5u, "MOV #imm wurde nicht propagiert.");
    require(trace[1].after.registers[1] == 8u, "ADD #imm wurde nicht propagiert.");
    require(trace[2].after.registers[2] == 8u, "Registerkopie wurde nicht propagiert.");
    require(trace[3].after.registers[2] == 8u, "NOP hat eine Konstante zerstoert.");
    require(!trace[4].after.registers[1].has_value() && !trace[4].after.registers[2].has_value(), "Unmodellierter Effekt wurde nicht konservativ verworfen.");

    std::cout << "KR-1801 Lokale Konstantenpropagation erfolgreich.\n";
    return EXIT_SUCCESS;
}
