#include "katana/runtime/exception.hpp"
#include "katana/runtime/platform_interrupt.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function> void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

} // namespace

int main() {
    using namespace katana::runtime;

    EventScheduler scheduler;
    Memory memory(256u, MemoryAlignmentPolicy::Strict);
    auto rtc_clock = std::make_shared<Sh4RtcClockDomain>(256u);
    Sh4Tmu tmu(scheduler, TmuTiming{1u, rtc_clock});
    Sh4Rtc rtc(scheduler, rtc_clock);
    Sh4Dmac dmac(scheduler, memory, DmaTiming{1u});
    InterruptController controller;
    PlatformInterruptRouter router(controller, tmu, rtc, dmac);
    const auto intc = map_sh4_interrupt_registers(memory, router);

    memory.write_u16(sh4_intc_p4_base, 0xFFFFu);
    memory.write_u16(sh4_intc_p4_base + 0x04u, 0xA765u);
    memory.write_u16(sh4_intc_area7_base + 0x08u, 0x1234u);
    memory.write_u16(sh4_intc_area7_base + 0x0Cu, 0x0B00u);
    require(memory.read_u16(sh4_intc_area7_base) == 0x4380u &&
                memory.read_u16(sh4_intc_p4_base + 0x04u) == 0xA765u &&
                memory.read_u16(sh4_intc_p4_base + 0x08u) == 0x1234u &&
                memory.read_u16(sh4_intc_p4_base + 0x0Cu) == 0x0B00u &&
                memory.read_u16(sh4_intc_p4_base + 0x10u) == 0u && router.tmu_level(0u) == 10u &&
                router.tmu_level(1u) == 7u && router.tmu_level(2u) == 6u &&
                router.rtc_level() == 5u && router.dma_level() == 11u &&
                router.scif_level() == 0u,
            "INTC ICR/IPRA/IPRB/IPRC/IPRD oder Area-7-Alias sind nicht registergenau.");
    require_failure([&] { memory.write_u16(sh4_intc_p4_base + 0x10u, 1u); },
                    "Read-only INTC-IPRD akzeptiert Schreibzugriffe.");
    require_failure([&] { memory.write_u32(sh4_intc_p4_base, 1u); },
                    "INTC akzeptiert eine falsche Zugriffsbreite.");
    require_failure([&] { static_cast<void>(memory.read_u16(sh4_intc_p4_base + 0x02u)); },
                    "INTC akzeptiert einen reservierten Registeroffset.");
    intc->reset();
    require(router.tmu_level(0u) == 0u && router.rtc_level() == 0u && router.dma_level() == 0u,
            "INTC-Reset hinterlaesst Interruptprioritaeten.");

    router.set_tmu_level(0u, 4u);
    router.set_rtc_level(3u);
    router.set_dma_level(5u);
    router.set_scif_level(4u);
    router.set_scif_pending(3u, true);
    router.set_external_pending(2u, true);

    tmu.write_constant(0u, 0u);
    tmu.write_counter(0u, 0u);
    tmu.write_control(0u, Sh4Tmu::underflow_interrupt_enable);
    tmu.write_start(1u);
    rtc.set_periodic_rate(RtcPeriodicRate::Every1Over256Second);
    rtc.start();
    memory.write_u8(0x00u, 0xA5u);
    dmac.write_source(0u, 0x00u);
    dmac.write_destination(0u, 0x40u);
    dmac.write_count(0u, 1u);
    dmac.write_control(0u, 0x00000410u | Sh4Dmac::interrupt_enable | Sh4Dmac::channel_enable);
    dmac.write_operation(Sh4Dmac::master_enable);
    const auto generated = scheduler.advance_to(4u, 6u);
    require(generated.processed_events == 6u,
            "Plattformquellen erzeugen ihre Scheduler-Ereignisse nicht deterministisch.");

    require(router.synchronize() == 5u && controller.pending_count() == 5u &&
                controller.pending(static_cast<InterruptSource>(PlatformInterruptSource::Tmu0)) &&
                controller.pending(
                    static_cast<InterruptSource>(PlatformInterruptSource::RtcPeriodic)) &&
                controller.pending(static_cast<InterruptSource>(PlatformInterruptSource::Dma0)) &&
                controller.pending(
                    static_cast<InterruptSource>(PlatformInterruptSource::ScifTransmit)) &&
                controller.pending(
                    static_cast<InterruptSource>(PlatformInterruptSource::ExternalLevel6)),
            "Plattformrouter spiegelt nicht alle gleichzeitig gesetzten Quellen.");
    router.set_scif_pending(3u, false);
    static_cast<void>(router.synchronize());
    require(!controller.pending(
                static_cast<InterruptSource>(PlatformInterruptSource::ScifTransmit)),
            "Quittierte SCIF-TXI-Quelle bleibt im zentralen Controller haengen.");

    CpuState cpu;
    cpu.vbr = 0x8C000000u;
    cpu.pc = 0x8C010000u;
    cpu.set_interrupt_mask(0u);
    require(router.accept(cpu) && cpu.intevt == 0x00000320u && cpu.spc == 0x8C010000u,
            "Gleiches Level wird nicht nach stabiler Quellprioritaet angenommen.");
    router.set_external_pending(2u, false);
    return_from_exception(cpu);
    require(router.accept(cpu) && cpu.intevt == 0x00000640u,
            "DMTE0 wird nach Freigabe der hoeheren externen Quelle nicht zugestellt.");
    dmac.write_control(0u, 0x00000410u | Sh4Dmac::interrupt_enable | Sh4Dmac::channel_enable);
    return_from_exception(cpu);
    static_cast<void>(router.synchronize());
    require(!controller.pending(static_cast<InterruptSource>(PlatformInterruptSource::Dma0)),
            "Quittiertes DMTE bleibt im zentralen Controller haengen.");

    cpu.set_interrupt_mask(3u);
    require(router.accept(cpu) && cpu.intevt == 0x00000400u,
            "TMU-Level oberhalb IMASK wird nicht angenommen.");
    tmu.write_control(0u, 0u);
    return_from_exception(cpu);
    static_cast<void>(router.synchronize());
    require(!router.accept(cpu) && controller.pending(static_cast<InterruptSource>(
                                       PlatformInterruptSource::RtcPeriodic)),
            "RTC auf IMASK-Level wird faelschlich angenommen oder verworfen.");
    rtc.acknowledge_periodic_interrupt();
    tmu.write_start(0u);
    rtc.stop();
    static_cast<void>(router.synchronize());
    require(controller.pending_count() == 0u,
            "Quittierte Timer-/RTC-Quellen werden nicht aus dem Controller entfernt.");

    router.set_dma_level(9u);
    dmac.reset();
    dmac.write_source(0u, 1u);
    dmac.write_destination(0u, 0x80u);
    dmac.write_count(0u, 1u);
    dmac.write_control(0u, 0x00000430u | Sh4Dmac::channel_enable);
    dmac.write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(8u, 4u));
    cpu.set_interrupt_mask(8u);
    require(dmac.address_error() && router.accept(cpu) && cpu.intevt == 0x000006C0u,
            "DMAE wird nicht als priorisierte Plattformausnahme zugestellt.");
    dmac.write_operation(0u);
    return_from_exception(cpu);
    dmac.reset();
    rtc.set_rtc_enabled(false);
    rtc.acknowledge_periodic_interrupt();
    static_cast<void>(router.synchronize());

    router.set_tmu_level(1u, 0xFFu);
    require(router.tmu_level(1u) == 15u, "TMU-Prioritaet wird nicht auf 15 begrenzt.");
    router.set_external_pending(0u, true);
    cpu.write_sr(cpu.read_sr() | sr_bl_mask);
    require(!router.accept(cpu), "BL sperrt eine externe Plattformleitung nicht.");
    cpu.write_sr(cpu.read_sr() & ~sr_bl_mask);
    cpu.set_interrupt_mask(1u);
    const auto level2_accepted = router.accept(cpu);
    require(level2_accepted && cpu.intevt == 0x000003A0u,
            "Externe ASIC-Leitung 0 wird nicht als Level 2/INTEVT 0x3A0 zugestellt.");
    router.set_external_pending(0u, false);
    return_from_exception(cpu);
    router.set_external_pending(1u, true);
    cpu.set_interrupt_mask(3u);
    require(router.accept(cpu) && cpu.intevt == 0x00000360u,
            "Externe ASIC-Leitung 1 wird nicht als Level 4/INTEVT 0x360 zugestellt.");
    router.set_external_pending(1u, false);
    return_from_exception(cpu);
    router.set_external_pending(2u, true);
    cpu.set_interrupt_mask(5u);
    require(router.accept(cpu) && cpu.intevt == 0x00000320u,
            "Externe ASIC-Leitung 2 wird nicht als Level 6/INTEVT 0x320 zugestellt.");
    router.set_external_pending(2u, false);
    return_from_exception(cpu);
    router.reset();
    require(controller.pending_count() == 0u && router.tmu_level(0u) == 0u &&
                router.rtc_level() == 0u && router.dma_level() == 0u &&
                router.scif_level() == 0u && !router.scif_pending(3u) &&
                !router.external_pending(2u),
            "Plattformrouter-Reset hinterlaesst Quellen oder Prioritaeten.");

    std::cout << "KR-3104 Plattform-Interruptintegration erfolgreich.\n";
    return 0;
}
