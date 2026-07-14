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

}

int main() {
    using namespace katana::runtime;

    InterruptController controller;
    controller.request(7u, 4u, 0x00000320u);
    controller.request(3u, 9u, 0x00000600u);
    controller.request(3u, 10u, 0x00000620u);
    require(
        controller.pending_count() == 2u &&
        controller.pending(3u) &&
        controller.pending(7u),
        "Interrupt-Anforderungen werden nicht deterministisch aktualisiert."
    );

    CpuState cpu;
    cpu.vbr = 0x8C000000u;
    cpu.pc = 0x8C010020u;
    cpu.r[15] = 0x8C00FFF0u;
    cpu.set_interrupt_mask(5u);
    cpu.sleeping = true;

    require(
        accept_pending_interrupt(cpu, controller),
        "Der hoechstpriorisierte zulassbare Interrupt wird nicht angenommen."
    );
    require(
        cpu.last_exception_cause == ExceptionCause::Interrupt &&
        cpu.intevt == 0x00000620u &&
        cpu.spc == 0x8C010020u &&
        cpu.pc == 0x8C000600u &&
        !cpu.sleeping &&
        controller.pending_count() == 1u &&
        controller.pending(7u),
        "Interrupt-Eintritt verliert Ereignis, Rueckkehr-PC oder Pending-Zustand."
    );

    return_from_exception(cpu);
    cpu.set_interrupt_mask(15u);
    require(
        !accept_pending_interrupt(cpu, controller) &&
        controller.pending(7u),
        "Ein durch IMASK gesperrter Interrupt wurde angenommen."
    );

    cpu.set_interrupt_mask(0u);
    cpu.write_sr(cpu.read_sr() | sr_bl_mask);
    require(
        !accept_pending_interrupt(cpu, controller),
        "Ein Interrupt wurde trotz gesetztem BL angenommen."
    );

    cpu.write_sr(cpu.read_sr() & ~sr_bl_mask);
    require(
        accept_pending_interrupt(cpu, controller) &&
        cpu.intevt == 0x00000320u &&
        controller.pending_count() == 0u,
        "Der verbleibende Interrupt wird nach Freigabe nicht angenommen."
    );

    controller.request(1u, 1u, 0x200u);
    require(controller.cancel(1u), "Ein Pending-Interrupt laesst sich nicht abbrechen.");
    require(!controller.cancel(1u), "Ein fehlender Interrupt gilt faelschlich als abgebrochen.");
    controller.request(2u, 2u, 0x220u);
    controller.clear();
    require(controller.pending_count() == 0u, "Interrupt-Controller laesst sich nicht leeren.");

    std::cout << "Priorisierter Interrupt-Controller erfolgreich.\n";
    return EXIT_SUCCESS;
}
