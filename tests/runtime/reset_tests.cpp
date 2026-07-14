#include "katana/runtime/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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

template <std::size_t Size>
bool all_zero(const std::array<std::uint32_t, Size>& values) {
    return std::all_of(
        values.begin(),
        values.end(),
        [](const std::uint32_t value) {
            return value == 0u;
        }
    );
}

bool scalar_state_is_zero(const katana::runtime::CpuState& cpu) {
    return
        cpu.pc == 0u &&
        cpu.pr == 0u &&
        cpu.gbr == 0u &&
        cpu.vbr == 0u &&
        cpu.ssr == 0u &&
        cpu.spc == 0u &&
        cpu.sgr == 0u &&
        cpu.dbr == 0u &&
        cpu.tra == 0u &&
        cpu.expevt == 0u &&
        cpu.intevt == 0u &&
        cpu.mach == 0u &&
        cpu.macl == 0u &&
        cpu.fpul == 0u &&
        cpu.fpscr == 0u &&
        cpu.read_sr() == 0u &&
        !cpu.t &&
        !cpu.s &&
        !cpu.q &&
        !cpu.m &&
        !cpu.trap_pending &&
        !cpu.sleeping;
}

}

int main() {
    using katana::runtime::CpuState;
    using katana::runtime::ResetState;
    using katana::runtime::reset_cpu;

    CpuState cpu;

    cpu.r.fill(0x11111111u);
    cpu.r_bank.fill(0x22222222u);
    cpu.fr.fill(0x33333333u);
    cpu.xf.fill(0x44444444u);
    cpu.pc = 0x55555555u;
    cpu.pr = 0x66666666u;
    cpu.gbr = 0x77777777u;
    cpu.vbr = 0x88888888u;
    cpu.ssr = 0x99999999u;
    cpu.spc = 0xAAAAAAAAu;
    cpu.sgr = 0xBBBBBBBBu;
    cpu.dbr = 0xCCCCCCCCu;
    cpu.tra = 0xDDDDDDDDu;
    cpu.expevt = 0xEEEEEEEEu;
    cpu.intevt = 0xFFFFFFFFu;
    cpu.mach = 0x12345678u;
    cpu.macl = 0x87654321u;
    cpu.fpul = 0x3F800000u;
    cpu.fpscr = 0x00040001u;
    cpu.write_sr(0x700083F3u);
    cpu.trap_pending = true;
    cpu.sleeping = true;
    cpu.memory.write_u32(16u, 0x89ABCDEFu);

    reset_cpu(cpu);

    require(
        all_zero(cpu.r) &&
        all_zero(cpu.r_bank) &&
        all_zero(cpu.fr) &&
        all_zero(cpu.xf),
        "Der Standard-Reset loescht nicht alle Registerbaenke."
    );

    require(
        scalar_state_is_zero(cpu),
        "Der Standard-Reset erzeugt keinen vollstaendig definierten Nullzustand."
    );

    require(
        cpu.memory.read_u32(16u) == 0x89ABCDEFu,
        "Ein CPU-Reset darf den Runtime-Speicher nicht loeschen."
    );

    ResetState configured;
    configured.program_counter = 0x8C010000u;
    configured.stack_pointer = 0x8C00FFF0u;
    configured.vector_base = 0x8C000000u;
    configured.status_register = 0xFFFFFFFFu;
    configured.fpscr = 0x00040001u;

    cpu.r.fill(0xA5A5A5A5u);
    cpu.r_bank.fill(0x5A5A5A5Au);
    cpu.fr.fill(0x7FC00001u);
    cpu.xf.fill(0x80000000u);
    cpu.pr = 1u;
    cpu.gbr = 2u;
    cpu.ssr = 3u;
    cpu.spc = 4u;
    cpu.sgr = 5u;
    cpu.dbr = 6u;
    cpu.tra = 7u;
    cpu.expevt = 8u;
    cpu.intevt = 9u;
    cpu.mach = 10u;
    cpu.macl = 11u;
    cpu.fpul = 12u;
    cpu.trap_pending = true;
    cpu.sleeping = true;

    reset_cpu(cpu, configured);

    require(
        cpu.pc == configured.program_counter &&
        cpu.r[15] == configured.stack_pointer &&
        cpu.vbr == configured.vector_base &&
        cpu.fpscr == configured.fpscr,
        "Konfigurierte Reset-Werte werden nicht uebernommen."
    );

    require(
        cpu.read_sr() == 0x700083F3u &&
        cpu.sr == 0x700083F3u &&
        cpu.m &&
        cpu.q &&
        cpu.s &&
        cpu.t,
        "Der konfigurierte Statusregisterwert wird nicht maskiert uebernommen."
    );

    require(
        std::all_of(
            cpu.r.begin(),
            cpu.r.begin() + 15,
            [](const std::uint32_t value) {
                return value == 0u;
            }
        ) &&
        all_zero(cpu.r_bank) &&
        all_zero(cpu.fr) &&
        all_zero(cpu.xf),
        "Der konfigurierte Reset laesst alte Registerwerte zurueck."
    );

    require(
        cpu.pr == 0u &&
        cpu.gbr == 0u &&
        cpu.ssr == 0u &&
        cpu.spc == 0u &&
        cpu.sgr == 0u &&
        cpu.dbr == 0u &&
        cpu.tra == 0u &&
        cpu.expevt == 0u &&
        cpu.intevt == 0u &&
        cpu.mach == 0u &&
        cpu.macl == 0u &&
        cpu.fpul == 0u &&
        !cpu.trap_pending &&
        !cpu.sleeping,
        "Der konfigurierte Reset bewahrt unerlaubt alten CPU-Zustand."
    );

    require(
        cpu.memory.read_u32(16u) == 0x89ABCDEFu,
        "Auch ein konfigurierter CPU-Reset muss den Speicher bewahren."
    );

    cpu.pc = 1u;
    cpu.r[15] = 2u;
    cpu.vbr = 3u;
    cpu.fpscr = 4u;
    cpu.write_sr(0u);

    reset_cpu(cpu, configured);

    require(
        cpu.pc == configured.program_counter &&
        cpu.r[15] == configured.stack_pointer &&
        cpu.vbr == configured.vector_base &&
        cpu.fpscr == configured.fpscr &&
        cpu.read_sr() == 0x700083F3u,
        "Wiederholte Resets mit derselben Konfiguration sind nicht deterministisch."
    );

    std::cout << "Deterministischer Runtime-Reset erfolgreich.\n";
    return EXIT_SUCCESS;
}