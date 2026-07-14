#include "generated_r0_indexed_program.cpp"

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

void prepare_cpu(
    katana_generated::CpuState& cpu,
    const std::uint32_t byte_store_base
) {
    cpu.r[0] = 0x00000020u;
    cpu.r[1] = 0xA1B2C3D4u;
    cpu.r[2] = byte_store_base;
    cpu.r[3] = 0x1122A1B2u;
    cpu.r[4] = 0x00000140u;
    cpu.r[5] = 0x89ABCDEFu;
    cpu.r[6] = 0x00000180u;

    cpu.r[7] = 0x00000200u;
    cpu.memory.write_u8(0x00000220u, 0x80u);
    cpu.r[9] = 0x00000240u;
    cpu.memory.write_u16(0x00000260u, 0x8001u);
    cpu.r[11] = 0x00000280u;
    cpu.memory.write_u32(0x000002A0u, 0x76543210u);

    cpu.r[13] = 0x00000300u;
    cpu.r[14] = 0x00000340u;
    cpu.memory.write_u8(0x00000360u, 0xFEu);

    cpu.mach = 0x13579BDFu;
    cpu.macl = 0x2468ACE0u;
    cpu.t = true;
    cpu.s = false;
    cpu.q = true;
    cpu.m = false;
}

void verify_common_result(const katana_generated::CpuState& cpu) {
    require(
        cpu.memory.read_u16(0x00000160u) == 0xA1B2u &&
        cpu.memory.read_u32(0x000001A0u) == 0x89ABCDEFu,
        "R0-indexierte Word- oder Long-Stores sind falsch."
    );
    require(
        cpu.r[8] == 0xFFFFFF80u &&
        cpu.r[10] == 0xFFFF8001u &&
        cpu.r[12] == 0x76543210u,
        "R0-indexierte Loads oder ihre Vorzeichenerweiterung sind falsch."
    );
    require(
        cpu.memory.read_u8(0x00000320u) == 0x20u &&
        cpu.r[0] == 0xFFFFFFFEu,
        "Die R0-Ueberlappungsfaelle sind falsch."
    );
    require(
        cpu.r[4] == 0x00000140u &&
        cpu.r[6] == 0x00000180u &&
        cpu.r[7] == 0x00000200u &&
        cpu.r[9] == 0x00000240u &&
        cpu.r[11] == 0x00000280u,
        "R0-indexierte Zugriffe haben Basisregister veraendert."
    );
    require(
        cpu.mach == 0x13579BDFu &&
        cpu.macl == 0x2468ACE0u &&
        cpu.t && !cpu.s && cpu.q && !cpu.m,
        "R0-indexierte Zugriffe haben fremden CPU-Zustand veraendert."
    );
}

void run_normal_case() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu, 0x00000100u);
    katana_generated::run(cpu);

    require(
        cpu.memory.read_u8(0x00000120u) == 0xD4u &&
        cpu.r[2] == 0x00000100u,
        "MOV.B mit R0-Index ist falsch."
    );
    verify_common_result(cpu);
}

void run_wraparound_case() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu, 0xFFFFFFF0u);
    katana_generated::run(cpu);

    require(
        cpu.memory.read_u8(0x00000010u) == 0xD4u &&
        cpu.r[2] == 0xFFFFFFF0u,
        "Die R0-indexierte Adresse verwendet kein 32-Bit-Wraparound."
    );
    verify_common_result(cpu);
}

void run_invalid_access_case() {
    katana_generated::CpuState cpu;
    prepare_cpu(cpu, runtime_memory_size - 16u);

    const auto old_r0 = cpu.r[0];
    const auto old_r1 = cpu.r[1];
    const auto old_r2 = cpu.r[2];

    bool threw = false;
    try {
        katana_generated::run(cpu);
    } catch (const katana::runtime::MemoryAccessError& error) {
        threw =
            error.reason() ==
                katana::runtime::MemoryAccessErrorReason::Unmapped &&
            error.operation() ==
                katana::runtime::MemoryAccessOperation::Write &&
            error.width() ==
                katana::runtime::MemoryAccessWidth::Byte;
    }

    require(
        threw,
        "Eine R0-indexierte Adresse ausserhalb des Speichers muss fehlschlagen."
    );
    require(
        cpu.r[0] == old_r0 &&
        cpu.r[1] == old_r1 &&
        cpu.r[2] == old_r2,
        "Ein fehlgeschlagener R0-indexierter Store hat Register veraendert."
    );
}

}

int main() {
    run_normal_case();
    run_wraparound_case();
    run_invalid_access_case();

    std::cout
        << "KR-1403 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";

    return EXIT_SUCCESS;
}
