#include "katana/runtime/system_asic.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {
void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename F> bool throws(F&& function) {
    try {
        function();
    } catch (...) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;
    EventScheduler scheduler;
    Memory bus(0u);
    auto rtc_clock = std::make_shared<Sh4RtcClockDomain>(256u);
    Sh4Tmu tmu(scheduler, TmuTiming{1u, rtc_clock});
    Sh4Rtc rtc(scheduler, rtc_clock);
    Memory dma_memory(256u);
    Sh4Dmac dmac(scheduler, dma_memory, DmaTiming{1u});
    InterruptController controller;
    PlatformInterruptRouter router(controller, tmu, rtc, dmac);
    const auto asic = map_dreamcast_system_asic(bus, router);

    bus.write_u32(0xA05F6930u, (1u << 3u) | (1u << 12u) | (1u << 14u) | (1u << 15u));
    bus.write_u32(0xA05F6934u, (1u << 0u) | (1u << 1u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::PvrVblank, 10u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::MapleDma, 10u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::GdromDma, 11u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::AicaDma, 12u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::GdromCommand, 13u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::AicaInterrupt, 14u));
    const auto advanced = scheduler.advance_to(14u, 6u);
    require(advanced.processed_events == 6u && asic->events().size() == 6u &&
                asic->events()[0].sequence == 1u && asic->events()[1].sequence == 2u &&
                asic->events().back().guest_cycle == 14u,
            "ASIC-Ereignisse sind nicht gastzeit- und einreihungsdeterministisch.");
    require(bus.read_u32(0x005F6900u) == 0x0000D008u && bus.read_u32(0x005F6904u) == 0x00000003u &&
                router.external_pending(2u),
            "PVR/Maple/GD-ROM/AICA-Ereignisse erreichen nicht denselben Status-/Maskenpfad.");
    static_cast<void>(router.synchronize());
    require(controller.pending(static_cast<InterruptSource>(PlatformInterruptSource::ExternalIrl9)),
            "Maskierte System-ASIC-Ereignisse erreichen IRL9 nicht.");
    bus.write_u32(0x805F6900u, 0x0000D008u);
    bus.write_u32(0x805F6904u, 0x00000003u);
    require(!router.external_pending(2u), "ACK loescht die gemeinsame ASIC-Leitung nicht.");
    require(throws([&] { static_cast<void>(bus.read_u32(0x005F690Cu)); }) &&
                throws([&] { bus.write_u32(0x005F693Cu, 1u); }) &&
                throws([&] { static_cast<void>(bus.read_u16(0x005F6900u)); }),
            "Unbekanntes oder falsch breites ASIC-MMIO war still erfolgreich.");
    require(throws([&] { asic->raise(SystemAsicEvent::PvrRenderDone, 9u); }),
            "Rueckwaerts laufende Gastzeit wurde akzeptiert.");
    std::cout << "KR-4603 Dreamcast-System-ASIC und Interruptintegration erfolgreich.\n";
}
