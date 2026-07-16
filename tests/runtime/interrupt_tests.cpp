#include "katana/runtime/exception.hpp"
#include "katana/runtime/interrupt.hpp"

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

    InterruptController controller;
    controller.request(7u, 4u, 0x00000320u);
    controller.request(3u, 9u, 0x00000600u);
    controller.request(3u, 10u, 0x00000620u);
    require(controller.pending_count() == 2u && controller.pending(3u) && controller.pending(7u),
            "Interrupt-Anforderungen werden nicht deterministisch aktualisiert.");

    CpuState cpu;
    cpu.vbr = 0x8C000000u;
    cpu.pc = 0x8C010020u;
    cpu.r[15] = 0x8C00FFF0u;
    cpu.set_interrupt_mask(5u);
    cpu.sleeping = true;

    require(accept_pending_interrupt(cpu, controller),
            "Der hoechstpriorisierte zulassbare Interrupt wird nicht angenommen.");
    require(cpu.last_exception_cause == ExceptionCause::Interrupt && cpu.intevt == 0x00000620u &&
                cpu.spc == 0x8C010020u && cpu.pc == 0x8C000600u && !cpu.sleeping &&
                controller.pending_count() == 1u && controller.pending(7u),
            "Interrupt-Eintritt verliert Ereignis, Rueckkehr-PC oder Pending-Zustand.");

    return_from_exception(cpu);
    cpu.set_interrupt_mask(15u);
    require(!accept_pending_interrupt(cpu, controller) && controller.pending(7u),
            "Ein durch IMASK gesperrter Interrupt wurde angenommen.");

    cpu.set_interrupt_mask(0u);
    cpu.write_sr(cpu.read_sr() | sr_bl_mask);
    require(!accept_pending_interrupt(cpu, controller),
            "Ein Interrupt wurde trotz gesetztem BL angenommen.");

    cpu.write_sr(cpu.read_sr() & ~sr_bl_mask);
    require(accept_pending_interrupt(cpu, controller) && cpu.intevt == 0x00000320u &&
                controller.pending_count() == 0u,
            "Der verbleibende Interrupt wird nach Freigabe nicht angenommen.");

    controller.request(1u, 1u, 0x200u);
    require(controller.cancel(1u), "Ein Pending-Interrupt laesst sich nicht abbrechen.");
    require(!controller.cancel(1u), "Ein fehlender Interrupt gilt faelschlich als abgebrochen.");
    controller.request(2u, 2u, 0x220u);
    controller.clear();
    require(controller.pending_count() == 0u, "Interrupt-Controller laesst sich nicht leeren.");

    {
        InterruptController equal_level;
        equal_level.request(9u, 8u, 0x900u);
        equal_level.request(2u, 8u, 0x200u);
        CpuState tied_cpu;
        tied_cpu.set_interrupt_mask(0u);
        require(accept_pending_interrupt(tied_cpu, equal_level) && tied_cpu.intevt == 0x200u &&
                    equal_level.pending(9u) && !equal_level.pending(2u),
                "Gleiche Interruptlevel werden nicht nach kleinerer Quell-ID aufgeloest.");
    }

    {
        InterruptController zero_level;
        zero_level.request(1u, 0u, 0x100u);
        CpuState zero_cpu;
        zero_cpu.set_interrupt_mask(0u);
        require(!accept_pending_interrupt(zero_cpu, zero_level) && zero_level.pending(1u),
                "Interruptlevel 0 wurde faelschlich als annehmbar behandelt.");
    }

    {
        InterruptController clamped;
        clamped.request(4u, 0xFFu, 0x400u);
        CpuState clamped_cpu;
        clamped_cpu.set_interrupt_mask(14u);
        require(accept_pending_interrupt(clamped_cpu, clamped) && clamped_cpu.intevt == 0x400u,
                "Interruptlevel oberhalb 15 wird nicht auf Level 15 begrenzt.");
    }

    {
        InterruptController updated;
        updated.request(1u, 12u, 0x120u);
        updated.request(2u, 7u, 0x220u);
        updated.request(1u, 3u, 0x130u);
        CpuState updated_cpu;
        updated_cpu.set_interrupt_mask(0u);
        require(updated.pending_count() == 2u && accept_pending_interrupt(updated_cpu, updated) &&
                    updated_cpu.intevt == 0x220u && updated.pending(1u),
                "Aktualisierung einer Quelle von hohem auf niedriges Level bleibt wirkungslos.");
    }

    {
        InterruptController pending_during_trap;
        pending_during_trap.request(6u, 5u, 0x600u);
        CpuState trap_cpu;
        trap_cpu.trap_pending = true;
        trap_cpu.set_interrupt_mask(0u);
        require(!trap_cpu.interrupts_blocked() &&
                    accept_pending_interrupt(trap_cpu, pending_during_trap) &&
                    trap_cpu.intevt == 0x600u,
                "trap_pending ohne BL sperrt einen ansonsten annehmbaren Interrupt.");
    }

    std::cout << "Priorisierter Interrupt-Controller erfolgreich.\n";
    return EXIT_SUCCESS;
}
