#include "generated_register_displacement_program.cpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t runtime_memory_size = 1024u * 1024u;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void prepare_cpu(katana_generated::CpuState& cpu, const std::uint32_t byte_store_base) {
    cpu.r[0] = 0xA1B2C3D4u;
    cpu.r[1] = byte_store_base;
    cpu.r[2] = 0x00000120u;
    cpu.r[3] = 0x89ABCDEFu;
    cpu.r[4] = 0x00000180u;

    cpu.r[5] = 0x00000200u;
    cpu.memory.write_u8(0x0000020Fu, 0x80u);

    cpu.r[6] = 0x00000220u;
    cpu.memory.write_u16(0x0000023Eu, 0x8001u);

    cpu.r[7] = 0x00000280u;
    cpu.memory.write_u32(0x000002BCu, 0x76543210u);

    cpu.r[8] = 0xDEADBEEFu;
    cpu.mach = 0x13579BDFu;
    cpu.macl = 0x2468ACE0u;
    cpu.t = true;
    cpu.s = true;
    cpu.q = false;
    cpu.m = true;
}

void verify_common_result(const katana_generated::CpuState& cpu) {
    require(cpu.r[0] == 0xFFFF8001u, "Byte- oder Word-Displacement-Load hat R0 falsch gesetzt.");

    require(cpu.memory.read_u16(0x0000013Eu) == 0xC3D4u,
            "MOV.W Displacement-Store ist nicht Little Endian.");

    require(cpu.memory.read_u32(0x000001BCu) == 0x89ABCDEFu && cpu.r[3] == 0x89ABCDEFu &&
                cpu.r[4] == 0x00000180u,
            "MOV.L Displacement-Store oder seine Registereffekte sind falsch.");

    require(cpu.r[5] == 0x00000200u && cpu.r[6] == 0x00000220u && cpu.r[7] == 0x00000280u &&
                cpu.r[8] == 0x76543210u,
            "Displacement-Loads haben Basisregister oder Long-Ziel falsch behandelt.");

    require(cpu.mach == 0x13579BDFu && cpu.macl == 0x2468ACE0u && cpu.t && cpu.s && !cpu.q && cpu.m,
            "Register-Displacements haben fremden CPU-Zustand veraendert.");
}

void run_normal_case() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu, 0x00000100u);

    katana_generated::run(cpu);

    require(cpu.memory.read_u8(0x0000010Fu) == 0xD4u && cpu.r[1] == 0x00000100u &&
                cpu.r[2] == 0x00000120u,
            "MOV.B Displacement-Store oder Basisregister sind falsch.");

    verify_common_result(cpu);
}

void run_wraparound_case() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu, 0xFFFFFFF1u);

    katana_generated::run(cpu);

    require(cpu.memory.read_u8(0u) == 0xD4u && cpu.r[1] == 0xFFFFFFF1u,
            "Die effektive Displacement-Adresse verwendet kein 32-Bit-Wraparound.");

    verify_common_result(cpu);
}

void run_invalid_access_case() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu, runtime_memory_size - 8u);

    const auto old_r0 = cpu.r[0];
    const auto old_r1 = cpu.r[1];

    katana_generated::run(cpu);

    require(cpu.trap_pending &&
                cpu.last_exception_cause == katana::runtime::ExceptionCause::BusErrorWrite &&
                cpu.tea == runtime_memory_size + 7u && cpu.spc == 0x8C010000u,
            "Ungueltiges Displacement erzeugt keine strukturierte Bus-Exception.");
    katana::runtime::return_from_exception(cpu);
    require(cpu.r[0] == old_r0 && cpu.r[1] == old_r1,
            "Ein fehlgeschlagener Displacement-Store hat Register veraendert.");
}

} // namespace

int main() {
    run_normal_case();
    run_wraparound_case();
    run_invalid_access_case();

    std::cout << "KR-1402 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
