#include "generated_gbr_displacement_program.cpp"

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

void set_preserved_state(katana_generated::CpuState& cpu) {
    cpu.mach = 0x13579BDFu;
    cpu.macl = 0x2468ACE0u;
    cpu.t = true;
    cpu.s = true;
    cpu.q = false;
    cpu.m = true;
}

void require_preserved_state(const katana_generated::CpuState& cpu) {
    require(cpu.mach == 0x13579BDFu && cpu.macl == 0x2468ACE0u && cpu.t && cpu.s && !cpu.q && cpu.m,
            "GBR-relative Zugriffe haben fremden CPU-Zustand veraendert.");
}

void run_normal_cases() {
    katana_generated::CpuState cpu;
    cpu.gbr = 0x00000100u;
    cpu.r[0] = 0xA1B2C3D4u;
    set_preserved_state(cpu);

    cpu.pc = 0x8C010030u;
    katana_generated::fn_8C010030(cpu);

    require(cpu.memory.read_u8(0x000001FFu) == 0xD4u &&
                cpu.memory.read_u16(0x000002FEu) == 0xC3D4u &&
                cpu.memory.read_u32(0x000004FCu) == 0xA1B2C3D4u,
            "GBR-relative Stores oder Little Endian sind falsch.");
    require(cpu.gbr == 0x00000100u, "GBR wurde durch einen Store veraendert.");

    cpu.memory.write_u8(0x000001FFu, 0x80u);
    cpu.r[0] = 0u;
    cpu.pc = 0x8C010040u;
    katana_generated::fn_8C010040(cpu);
    require(cpu.r[0] == 0xFFFFFF80u, "GBR-Byte-Load erweitert falsch.");

    cpu.memory.write_u16(0x000002FEu, 0x8001u);
    cpu.r[0] = 0u;
    cpu.pc = 0x8C010048u;
    katana_generated::fn_8C010048(cpu);
    require(cpu.r[0] == 0xFFFF8001u, "GBR-Word-Load erweitert falsch.");

    cpu.memory.write_u32(0x000004FCu, 0x76543210u);
    cpu.r[0] = 0u;
    cpu.pc = 0x8C010050u;
    katana_generated::fn_8C010050(cpu);
    require(cpu.r[0] == 0x76543210u, "GBR-Long-Load ist falsch.");

    require(cpu.gbr == 0x00000100u, "GBR wurde durch einen Load veraendert.");
    require_preserved_state(cpu);
}

void run_wraparound_case() {
    katana_generated::CpuState cpu;
    cpu.gbr = 0xFFFFFF01u;
    cpu.r[0] = 0xA1B2C3D4u;
    set_preserved_state(cpu);

    cpu.pc = 0x8C010030u;
    katana_generated::fn_8C010030(cpu);

    require(cpu.memory.read_u8(0u) == 0xD4u && cpu.memory.read_u16(0x000000FFu) == 0xC3D4u &&
                cpu.memory.read_u32(0x000002FDu) == 0xA1B2C3D4u,
            "GBR-relative Adressen verwenden kein 32-Bit-Wraparound.");
    require(cpu.gbr == 0xFFFFFF01u, "GBR wurde im Wraparound-Fall veraendert.");
}

void run_invalid_access_case() {
    katana_generated::CpuState cpu;
    cpu.gbr = runtime_memory_size - 128u;
    cpu.r[0] = 0xA1B2C3D4u;

    const auto old_gbr = cpu.gbr;
    const auto old_r0 = cpu.r[0];

    cpu.pc = 0x8C010030u;
    katana_generated::fn_8C010030(cpu);

    require(cpu.trap_pending &&
                cpu.last_exception_cause == katana::runtime::ExceptionCause::AddressErrorWrite &&
                cpu.tea == runtime_memory_size + 127u && cpu.spc == 0x8C010030u,
            "Eine ungueltige GBR-Adresse erzeugt keine Bus-Exception.");
    katana::runtime::return_from_exception(cpu);
    require(cpu.gbr == old_gbr && cpu.r[0] == old_r0,
            "Ein fehlgeschlagener GBR-Store hat Register veraendert.");
}

} // namespace

int main() {
    run_normal_cases();
    run_wraparound_case();
    run_invalid_access_case();

    std::cout << "KR-1404 End-to-End-Semantik wurde erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
