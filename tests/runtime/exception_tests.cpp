#include "katana/runtime/exception.hpp"

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

} // namespace

int main() {
    using namespace katana::runtime;

    struct MetadataVector {
        ExceptionCause input;
        ExceptionCause normalized;
        std::uint32_t event;
        std::uint32_t vector;
        bool interrupt;
        bool slot;
    };
    constexpr std::array metadata_vectors{
        MetadataVector{ExceptionCause::Trap,
                       ExceptionCause::Trap,
                       event_trapa,
                       general_exception_vector,
                       false,
                       false},
        MetadataVector{ExceptionCause::IllegalInstruction,
                       ExceptionCause::IllegalInstruction,
                       event_illegal_instruction,
                       general_exception_vector,
                       false,
                       false},
        MetadataVector{ExceptionCause::SlotIllegalInstruction,
                       ExceptionCause::SlotIllegalInstruction,
                       event_slot_illegal_instruction,
                       general_exception_vector,
                       false,
                       true},
        MetadataVector{ExceptionCause::FpuDisabled,
                       ExceptionCause::FpuDisabled,
                       event_fpu_disabled,
                       general_exception_vector,
                       false,
                       false},
        MetadataVector{ExceptionCause::SlotFpuDisabled,
                       ExceptionCause::SlotFpuDisabled,
                       event_slot_fpu_disabled,
                       general_exception_vector,
                       false,
                       true},
        MetadataVector{ExceptionCause::AddressErrorRead,
                       ExceptionCause::AddressErrorRead,
                       event_address_error_read,
                       general_exception_vector,
                       false,
                       false},
        MetadataVector{ExceptionCause::AddressErrorWrite,
                       ExceptionCause::AddressErrorWrite,
                       event_address_error_write,
                       general_exception_vector,
                       false,
                       false},
        MetadataVector{ExceptionCause::BusErrorRead,
                       ExceptionCause::AddressErrorRead,
                       event_address_error_read,
                       general_exception_vector,
                       false,
                       false},
        MetadataVector{ExceptionCause::BusErrorWrite,
                       ExceptionCause::AddressErrorWrite,
                       event_address_error_write,
                       general_exception_vector,
                       false,
                       false},
        MetadataVector{ExceptionCause::Interrupt,
                       ExceptionCause::Interrupt,
                       0x00000320u,
                       interrupt_vector,
                       true,
                       false},
    };
    for (const auto& vector : metadata_vectors) {
        const auto metadata = exception_metadata(vector.input, 0x00000320u);
        require(metadata.cause == vector.normalized && metadata.event_code == vector.event &&
                    metadata.vector_offset == vector.vector &&
                    metadata.interrupt == vector.interrupt,
                "Exception-Metadatentabelle weicht vom unabhaengigen Referenzvektor ab.");
        CpuState state;
        state.vbr = 0x80000000u;
        state.r[15] = 0x8C00FFF0u;
        state.write_sr(sr_t_mask | (3u << 4u));
        enter_exception(state,
                        {vector.input,
                         0x00000320u,
                         0xDEADu,
                         0x8C010000u,
                         0xAABBCCDDu,
                         vector.interrupt,
                         vector.slot});
        require(
            state.last_exception_cause == vector.normalized &&
                state.pc == state.vbr + vector.vector && state.spc == 0x8C010000u &&
                state.ssr == (sr_t_mask | (3u << 4u)) && state.sgr == 0x8C00FFF0u &&
                state.tea == 0xAABBCCDDu && state.exception_in_delay_slot == vector.slot &&
                state.trap_pending &&
                (vector.interrupt ? state.intevt == vector.event : state.expevt == vector.event),
            "Exception-Eintritt verliert Event, Vektor oder gesicherten Gastzustand.");
    }

    CpuState cpu;
    cpu.vbr = 0x8C000000u;
    cpu.r[0] = 0x11111111u;
    cpu.r_bank[0] = 0x22222222u;
    cpu.r[15] = 0x8C00FFF0u;
    cpu.write_sr(sr_t_mask | (3u << 4u));

    enter_exception(cpu,
                    ExceptionRequest{ExceptionCause::IllegalInstruction,
                                     event_illegal_instruction,
                                     general_exception_vector,
                                     0x8C010000u});

    require(cpu.ssr == (sr_t_mask | (3u << 4u)) && cpu.spc == 0x8C010000u && cpu.sgr == 0x8C00FFF0u,
            "Exception-Eintritt sichert den CPU-Zustand falsch.");
    require(cpu.expevt == event_illegal_instruction && cpu.pc == 0x8C000100u && cpu.trap_pending &&
                cpu.privileged_mode() && cpu.register_bank_selected() && cpu.interrupts_blocked() &&
                cpu.r[0] == 0x22222222u,
            "Exception-Eintritt setzt Ereignis, Vektor oder SR falsch.");

    return_from_exception(cpu);
    require(cpu.pc == 0x8C010000u && cpu.read_sr() == (sr_t_mask | (3u << 4u)) &&
                !cpu.trap_pending && cpu.r[0] == 0x11111111u,
            "Exception-Rueckkehr restauriert PC, SR oder Registerbank falsch.");

    MemoryAccessError misaligned(MemoryAccessErrorReason::Misaligned,
                                 MemoryAccessOperation::Read,
                                 0x8C010003u,
                                 MemoryAccessWidth::Word,
                                 "ram");
    enter_memory_exception(cpu, misaligned, 0x8C020000u, 0x8C01FFFEu);
    require(cpu.last_exception_cause == ExceptionCause::AddressErrorRead &&
                cpu.expevt == event_address_error_read && cpu.tea == 0x8C010003u &&
                cpu.spc == 0x8C01FFFEu && cpu.exception_in_delay_slot,
            "Adressfehler im Delay Slot verliert Ursache, TEA oder Owner-PC.");

    return_from_exception(cpu);
    MemoryAccessError unmapped(MemoryAccessErrorReason::Unmapped,
                               MemoryAccessOperation::Write,
                               0xDEADBEEFu,
                               MemoryAccessWidth::Halfword);
    enter_memory_exception(cpu, unmapped, 0x8C030000u);
    require(cpu.last_exception_cause == ExceptionCause::BusErrorWrite &&
                cpu.expevt == event_address_error_write && cpu.tea == 0xDEADBEEFu &&
                cpu.spc == 0x8C030000u && !cpu.exception_in_delay_slot,
            "Busfehler wird nicht strukturiert in den Exception-Pfad ueberfuehrt.");

    return_from_exception(cpu);
    raise_fpu_disabled(cpu, 0x8C040002u, 0x8C040000u);
    require(cpu.last_exception_cause == ExceptionCause::SlotFpuDisabled &&
                cpu.expevt == event_slot_fpu_disabled && cpu.spc == 0x8C040000u &&
                cpu.exception_in_delay_slot,
            "FPU-Sperre im Delay Slot verliert Eventcode oder Owner-PC.");

    return_from_exception(cpu);
    raise_fpu_disabled(cpu, 0x8C050000u);
    require(cpu.last_exception_cause == ExceptionCause::FpuDisabled &&
                cpu.expevt == event_fpu_disabled && cpu.spc == 0x8C050000u &&
                !cpu.exception_in_delay_slot,
            "FPU-Sperre ausserhalb eines Delay Slots wird falsch gemeldet.");

    std::cout << "Strukturierter Exception-Eintritt erfolgreich.\n";
    return EXIT_SUCCESS;
}
