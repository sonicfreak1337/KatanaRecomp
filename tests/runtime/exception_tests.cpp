#include "katana/runtime/exception.hpp"

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
    using namespace katana::runtime;

    CpuState cpu;
    cpu.vbr = 0x8C000000u;
    cpu.r[0] = 0x11111111u;
    cpu.r_bank[0] = 0x22222222u;
    cpu.r[15] = 0x8C00FFF0u;
    cpu.write_sr(sr_t_mask | (3u << 4u));

    enter_exception(
        cpu,
        ExceptionRequest{
            ExceptionCause::IllegalInstruction,
            event_illegal_instruction,
            general_exception_vector,
            0x8C010000u
        }
    );

    require(
        cpu.ssr == (sr_t_mask | (3u << 4u)) &&
        cpu.spc == 0x8C010000u &&
        cpu.sgr == 0x8C00FFF0u,
        "Exception-Eintritt sichert den CPU-Zustand falsch."
    );
    require(
        cpu.expevt == event_illegal_instruction &&
        cpu.pc == 0x8C000100u &&
        cpu.trap_pending &&
        cpu.privileged_mode() &&
        cpu.register_bank_selected() &&
        cpu.interrupts_blocked() &&
        cpu.r[0] == 0x22222222u,
        "Exception-Eintritt setzt Ereignis, Vektor oder SR falsch."
    );

    return_from_exception(cpu);
    require(
        cpu.pc == 0x8C010000u &&
        cpu.read_sr() == (sr_t_mask | (3u << 4u)) &&
        !cpu.trap_pending &&
        cpu.r[0] == 0x11111111u,
        "Exception-Rueckkehr restauriert PC, SR oder Registerbank falsch."
    );

    MemoryAccessError misaligned(
        MemoryAccessErrorReason::Misaligned,
        MemoryAccessOperation::Read,
        0x8C010003u,
        MemoryAccessWidth::Word,
        "ram"
    );
    enter_memory_exception(cpu, misaligned, 0x8C020000u, 0x8C01FFFEu);
    require(
        cpu.last_exception_cause == ExceptionCause::AddressErrorRead &&
        cpu.expevt == event_address_error_read &&
        cpu.tea == 0x8C010003u &&
        cpu.spc == 0x8C01FFFEu &&
        cpu.exception_in_delay_slot,
        "Adressfehler im Delay Slot verliert Ursache, TEA oder Owner-PC."
    );

    return_from_exception(cpu);
    MemoryAccessError unmapped(
        MemoryAccessErrorReason::Unmapped,
        MemoryAccessOperation::Write,
        0xDEADBEEFu,
        MemoryAccessWidth::Halfword
    );
    enter_memory_exception(cpu, unmapped, 0x8C030000u);
    require(
        cpu.last_exception_cause == ExceptionCause::BusErrorWrite &&
        cpu.expevt == event_address_error_write &&
        cpu.tea == 0xDEADBEEFu &&
        cpu.spc == 0x8C030000u &&
        !cpu.exception_in_delay_slot,
        "Busfehler wird nicht strukturiert in den Exception-Pfad ueberfuehrt."
    );

    std::cout << "Strukturierter Exception-Eintritt erfolgreich.\n";
    return EXIT_SUCCESS;
}
