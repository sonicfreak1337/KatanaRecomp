#include "katana/runtime/runtime.hpp"

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

}

int main() {
    require(
        katana::runtime::abi_version == 1u,
        "Die Runtime-ABI besitzt nicht Version 1."
    );

    katana::runtime::Memory memory(16u);
    require(
        memory.size() == 16u,
        "Die Runtime-Speichergroesse ist falsch."
    );

    memory.write_u32(4u, 0x89ABCDEFu);
    require(
        memory.read_u8(4u) == 0xEFu &&
        memory.read_u16(4u) == 0xCDEFu &&
        memory.read_u32(4u) == 0x89ABCDEFu,
        "Little-Endian-Speicherzugriffe sind inkonsistent."
    );

    memory.write_u8(0u, 0x80u);
    memory.write_u16(1u, 0x8000u);
    require(
        memory.read_s8(0u) == 0xFFFFFF80u &&
        memory.read_s16(1u) == 0xFFFF8000u,
        "Vorzeichenerweiterte Runtime-Loads sind falsch."
    );

    bool bounds_failed = false;
    try {
        static_cast<void>(memory.read_u32(14u));
    } catch (const std::out_of_range&) {
        bounds_failed = true;
    }
    require(
        bounds_failed,
        "Runtime-Speichergrenzen werden nicht geprueft."
    );

    katana::runtime::CpuState cpu;
    require(
        cpu.pc == 0u &&
        cpu.pr == 0u &&
        cpu.memory.size() == 1024u * 1024u,
        "Der zentrale CPU-Zustand besitzt keinen deterministischen Grundzustand."
    );

    cpu.r[0] = 0x11111111u;
    cpu.r_bank[0] = 0x22222222u;
    cpu.write_sr(0x20000303u);
    require(
        cpu.r[0] == 0x22222222u &&
        cpu.r_bank[0] == 0x11111111u &&
        cpu.m &&
        cpu.q &&
        cpu.s &&
        cpu.t &&
        (cpu.read_sr() & 0x20000303u) == 0x20000303u,
        "SR-Schreiben schaltet Flags oder Registerbank falsch."
    );

    cpu.write_sr(0u);
    require(
        cpu.r[0] == 0x11111111u &&
        cpu.r_bank[0] == 0x22222222u &&
        !cpu.m &&
        !cpu.q &&
        !cpu.s &&
        !cpu.t,
        "Rueckschalten der Runtime-Registerbank ist falsch."
    );

    bool call_failed = false;
    try {
        katana::runtime::unresolved_call(cpu, 0x12345678u);
    } catch (const std::runtime_error& error) {
        call_failed =
            std::string(error.what()) == "Nicht aufgeloester Aufruf";
    }
    require(
        call_failed,
        "Ungeloeste Runtime-Aufrufe schlagen nicht sichtbar fehl."
    );

    bool jump_failed = false;
    try {
        katana::runtime::unresolved_jump(cpu, 0x12345678u);
    } catch (const std::runtime_error& error) {
        jump_failed =
            std::string(error.what()) == "Nicht aufgeloester Sprung";
    }
    require(
        jump_failed,
        "Ungeloeste Runtime-Spruenge schlagen nicht sichtbar fehl."
    );

    std::cout << "Katana-Runtime-Grundlage erfolgreich.\n";
    return EXIT_SUCCESS;
}