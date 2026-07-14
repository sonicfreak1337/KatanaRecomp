#include "generated_pc_relative_edge_program.cpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void run_invalid_access_case() {
    katana_generated::CpuState cpu;
    cpu.r[3] = 0xA1B2C3D4u;
    cpu.pc = 0x00000200u;
    katana_generated::fn_00000200(cpu);
    require(
        cpu.trap_pending &&
        cpu.last_exception_cause ==
            katana::runtime::ExceptionCause::BusErrorRead &&
        cpu.spc == 0x00000200u,
        "Ein PC-relativer Zugriff erzeugt keine strukturierte Bus-Exception."
    );
    katana::runtime::return_from_exception(cpu);
    require(
        cpu.r[3] == 0xA1B2C3D4u,
        "Ein fehlgeschlagener PC-relativer Load hat das Zielregister veraendert."
    );
}

void run_zero_address_case() {
    katana_generated::CpuState cpu;
    cpu.memory.write_u32(0u, 0x89ABCDEFu);
    cpu.pc = 0x00000210u;
    katana_generated::fn_00000210(cpu);
    require(
        cpu.r[4] == 0x89ABCDEFu,
        "Ein PC-relativer Load von Adresse null wurde falsch ausgefuehrt."
    );
}

}

int main() {
    run_invalid_access_case();
    run_zero_address_case();
    std::cout << "KR-1405 synthetische Codegen-Grenzfaelle erfolgreich.\n";
    return EXIT_SUCCESS;
}
