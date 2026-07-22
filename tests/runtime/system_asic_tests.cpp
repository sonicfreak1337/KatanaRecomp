#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/system_asic.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
    std::uint32_t channel2_destination = 0u;
    std::uint32_t channel2_length = 0u;
    std::size_t channel2_starts = 0u;
    const auto system_bus = map_dreamcast_system_bus_control(
        bus, [&](const std::uint32_t destination, const std::uint32_t length) {
            channel2_destination = destination;
            channel2_length = length;
            ++channel2_starts;
        });
    const auto asic = map_dreamcast_system_asic(bus, router);
    require(bus.read_u32(0x005F6800u) == 0x10000000u,
            "Channel-2-Ziel besitzt nicht den festen TA/PVR-Area-4-Resetwert.");

    bus.write_u32(0xA05F6800u, 0x11FF0000u);
    bus.write_u32(0xA05F6804u, 0xFFFFFFFFu);
    bus.write_u32(0x805F6810u, 0x0CFF0000u);
    bus.write_u32(0x005F6814u, 0x0CFF0000u);
    bus.write_u32(0xA05F6844u, 0xFFFFFFFFu);
    bus.write_u32(0xA05F6848u, 0xFFFFFFFFu);
    bus.write_u32(0xA05F684Cu, 0xFFFFFFFFu);
    bus.write_u32(0xA05F6884u, 1u);
    bus.write_u32(0xA05F6888u, 1u);
    bus.write_u32(0xA05F68A0u, 0xFFFFFFFFu);
    bus.write_u32(0xA05F68A4u, 0u);
    bus.write_u32(0xA05F68ACu, 0u);
    require(bus.read_u32(0x005F6800u) == 0x11FF0000u && bus.read_u32(0x005F6804u) == 0x00FFFFE0u &&
                bus.read_u32(0xA05F6810u) == 0x0CFF0000u &&
                bus.read_u32(0x805F6814u) == 0x0CFF0000u && bus.read_u32(0x005F6844u) == 0x1Fu &&
                bus.read_u32(0x005F6848u) == 0xFu && bus.read_u32(0x005F684Cu) == 3u &&
                bus.read_u32(0x005F6884u) == 1u && bus.read_u32(0x005F6888u) == 1u &&
                bus.read_u32(0x005F68A0u) == 0x80000000u,
            "Systembus-Steuerregister maskieren Werte oder spiegeln Direktsegmente falsch.");
    require(bus.read_u32(0x005F6860u) == 0u && bus.read_u32(0x005F6880u) == 8u &&
                bus.read_u32(0x005F688Cu) == 0u && bus.read_u32(0x005F689Cu) == 0xBu,
            "Read-only-Systembusstatus besitzt falsche Resetwerte.");
    require(throws([&] { bus.write_u32(0x005F6860u, 1u); }) &&
                throws([&] { bus.write_u32(0x005F6880u, 1u); }) &&
                throws([&] { static_cast<void>(bus.read_u32(0x005F6890u)); }) &&
                throws([&] { bus.write_u32(0x005F68A4u, 1u); }) &&
                throws([&] { bus.write_u32(0x005F6820u, 1u); }) &&
                throws([&] { static_cast<void>(bus.read_u16(0x005F6800u)); }) &&
                throws([&] { static_cast<void>(bus.read_u32(0x005F680Cu)); }) &&
                throws([&] { static_cast<void>(bus.read_u32(0x005F7C00u)); }),
            "Systembus-Zugriffsrechte, DMA-Start-Gate oder reservierte Offsets sind offen.");
    bus.write_u32(0x005F6808u, 1u);
    require(channel2_starts == 1u && channel2_destination == 0x11FF0000u &&
                channel2_length == 0x00FFFFE0u && bus.read_u32(0x005F6808u) == 1u &&
                throws([&] { bus.write_u32(0x005F6808u, 1u); }),
            "PVR-DMA startet nicht ueber den gemeinsamen Systembus-Channel-2-Zustand.");
    system_bus->complete_channel2();
    require(bus.read_u32(0x005F6804u) == 0u && bus.read_u32(0x005F6808u) == 0u,
            "PVR-DMA-Completion setzt Systembus-Laenge oder Startstatus nicht zurueck.");
    bus.write_u32(0x005F6890u, 0x7611u);
    require(system_bus->system_reset_requests() == 1u &&
                bus.read_u32(0x005F6800u) == 0x10000000u,
            "Systemreset-Schluessel setzt den Steuerblock nicht deterministisch zurueck.");
    static_assert(static_cast<std::uint16_t>(SystemAsicEvent::PvrDma) == 0x000Bu,
                  "Separater PVR-DMA verwendet nicht das reale ASIC-Bit 11.");
    static_assert(static_cast<std::uint16_t>(SystemAsicEvent::Channel2Dma) == 0x0013u,
                  "SH-4-Channel-2-DMA verwendet nicht das reale ASIC-Bit 19.");

    bus.write_u32(0xA05F6930u,
                  (1u << 3u) | (1u << 11u) | (1u << 12u) | (1u << 14u) | (1u << 15u) |
                      (1u << 19u));
    bus.write_u32(0xA05F6934u, (1u << 0u) | (1u << 1u));
    bus.write_u32(0xA05F6940u, 0xFFFFFFFFu);
    bus.write_u32(0xA05F6944u, 0xFFFFFFFFu);
    bus.write_u32(0xA05F6950u, 0xFFFFFFFFu);
    bus.write_u32(0xA05F6954u, 0xFFFFFFFFu);
    require(bus.read_u32(0x005F6940u) == 0x003FFFFFu && bus.read_u32(0x005F6944u) == 0xFu &&
                bus.read_u32(0x005F6950u) == 0x003FFFFFu && bus.read_u32(0x005F6954u) == 0xFu,
            "PVR-/G2-DMA-Triggermasken sind nicht vollstaendig abgedeckt.");
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::PvrVblank, 10u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::MapleDma, 10u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::GdromDma, 11u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::AicaDma, 12u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::PvrDma, 12u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::GdromCommand, 13u));
    static_cast<void>(asic->schedule(scheduler, SystemAsicEvent::AicaInterrupt, 14u));
    const auto advanced = scheduler.advance_to(14u, 7u);
    require(advanced.processed_events == 7u && asic->events().size() == 7u &&
                asic->events()[0].sequence == 1u && asic->events()[1].sequence == 2u &&
                asic->events().back().guest_cycle == 14u,
            "ASIC-Ereignisse sind nicht gastzeit- und einreihungsdeterministisch.");
    require(bus.read_u32(0x005F6900u) == 0x0000D808u && bus.read_u32(0x005F6904u) == 0x00000003u &&
                router.external_pending(2u),
            "PVR/Maple/GD-ROM/AICA-Ereignisse erreichen nicht denselben Status-/Maskenpfad.");
    static_cast<void>(router.synchronize());
    require(controller.pending(
                static_cast<InterruptSource>(PlatformInterruptSource::ExternalLevel6)),
            "Maskierte System-ASIC-Ereignisse erreichen die Level-6-Leitung nicht.");
    bus.write_u32(0x805F6900u, 0x0000D808u);
    bus.write_u32(0x805F6904u, 0x00000003u);
    require(!router.external_pending(2u), "ACK loescht die gemeinsame ASIC-Leitung nicht.");
    require(throws([&] { static_cast<void>(bus.read_u32(0x005F690Cu)); }) &&
                throws([&] { bus.write_u32(0x005F693Cu, 1u); }) &&
                throws([&] { bus.write_u32(0x005F6948u, 1u); }) &&
                throws([&] { static_cast<void>(bus.read_u16(0x005F6900u)); }),
            "Unbekanntes oder falsch breites ASIC-MMIO war still erfolgreich.");
    require(throws([&] { asic->raise(SystemAsicEvent::PvrRenderDone, 9u); }),
            "Rueckwaerts laufende Gastzeit wurde akzeptiert.");

    DreamcastRuntimeBootImage runtime_boot;
    const std::vector<std::uint8_t> disc_sector(dreamcast_data_sector_size, 0u);
    runtime_boot.source = std::make_shared<MemoryDiscSource>(
        std::span<const std::uint8_t>(disc_sector), "synthetic-channel2-ta-disc");
    runtime_boot.system_bootstrap.resize(dreamcast_system_bootstrap_size, 0u);
    runtime_boot.boot_file = {0x09u, 0x00u};
    runtime_boot.repeated_bootstrap_reads_match = true;
    runtime_boot.repeated_reads_match = true;
    CpuState runtime_cpu;
    auto runtime = initialize_dreamcast_runtime(runtime_cpu, runtime_boot);

    runtime.pvr_registers->write(pvr_register::TaIspBase, 0x00200000u);
    runtime.pvr_registers->write(pvr_register::TaNextOpbInit, 0x00100000u);
    runtime.pvr_registers->write(pvr_register::TaInit, 0x80000000u);
    constexpr std::uint32_t channel2_source = 0x0D002000u;
    runtime_cpu.memory.write_u32(channel2_source, 0x40000000u);
    for (std::uint32_t offset = 4u; offset < 64u; offset += 4u)
        runtime_cpu.memory.write_u32(channel2_source + offset, 0u);
    runtime.dmac->write_source(2u, channel2_source);
    runtime.dmac->write_count(2u, 2u);
    runtime.dmac->write_control(2u, 0x000012C1u);
    require(runtime.dmac->operation() == dreamcast_bios_handoff_dmaor,
            "Disc-Handoff stellt DMAOR.DDT fuer Channel 2 nicht bereit.");
    runtime.system_bus_control->write(system_bus_register::Channel2Destination, 0u);
    runtime.system_bus_control->write(system_bus_register::Channel2Length, 64u);
    runtime.system_bus_control->write(system_bus_register::Channel2Start, 1u);
    const auto channel2_run = runtime.scheduler->advance_by(64u, 2u);
    const auto event_count = [&runtime](const SystemAsicEvent event) {
        return std::count_if(runtime.system_asic->events().begin(),
                             runtime.system_asic->events().end(),
                             [event](const auto& record) { return record.event == event; });
    };
    require(channel2_run.processed_events == 2u &&
                runtime.pvr_ta_fifo->metrics().packets == 2u &&
                runtime.pvr_ta_fifo->metrics().list_completions == 1u &&
                event_count(SystemAsicEvent::PvrOpaqueList) == 1u &&
                event_count(SystemAsicEvent::Channel2Dma) == 1u &&
                event_count(SystemAsicEvent::PvrIllegalAddress) == 0u &&
                runtime.dmac->count(2u) == 0u &&
                runtime.dmac->source(2u) == channel2_source + 64u &&
                (runtime.dmac->control(2u) & Sh4Dmac::transfer_end) != 0u &&
                runtime.system_bus_control->read(system_bus_register::Channel2Length) == 0u &&
                runtime.system_bus_control->read(system_bus_register::Channel2Start) == 0u,
            "Reales RS=2/32-Byte/Burst-Channel-2-DMA erreicht TA-Liste oder ASIC nicht.");

    const auto packets_after_success = runtime.pvr_ta_fifo->metrics().packets;
    const auto illegal_before_direction = event_count(SystemAsicEvent::PvrIllegalAddress);
    runtime.dmac->reset();
    runtime.dmac->write_source(2u, channel2_source);
    runtime.dmac->write_count(2u, 1u);
    runtime.dmac->write_control(2u, 0x000013C1u);
    runtime.dmac->write_operation(dreamcast_bios_handoff_dmaor);
    runtime.system_bus_control->write(system_bus_register::Channel2Length, 32u);
    runtime.system_bus_control->write(system_bus_register::Channel2Start, 1u);
    require(runtime.pvr_ta_fifo->metrics().packets == packets_after_success &&
                event_count(SystemAsicEvent::PvrIllegalAddress) ==
                    illegal_before_direction + 1u &&
                runtime.dmac->address_error(),
            "Channel 2 akzeptiert die falsche External-Device-nach-Memory-Richtung.");

    const auto illegal_before_cycle_steal = event_count(SystemAsicEvent::PvrIllegalAddress);
    runtime.dmac->reset();
    runtime.dmac->write_source(2u, channel2_source);
    runtime.dmac->write_count(2u, 1u);
    runtime.dmac->write_control(2u, 0x00001241u);
    runtime.dmac->write_operation(dreamcast_bios_handoff_dmaor);
    runtime.system_bus_control->write(system_bus_register::Channel2Length, 32u);
    runtime.system_bus_control->write(system_bus_register::Channel2Start, 1u);
    require(runtime.pvr_ta_fifo->metrics().packets == packets_after_success &&
                event_count(SystemAsicEvent::PvrIllegalAddress) ==
                    illegal_before_cycle_steal + 1u &&
                runtime.dmac->address_error(),
            "Channel 2 akzeptiert Cycle-Steal statt des TA-Burstvertrags.");

    const auto illegal_before_area3_end = event_count(SystemAsicEvent::PvrIllegalAddress);
    runtime.dmac->reset();
    runtime.dmac->write_source(2u, 0x0FFFFFE0u);
    runtime.dmac->write_count(2u, 2u);
    runtime.dmac->write_control(2u, 0x000012C1u);
    runtime.dmac->write_operation(dreamcast_bios_handoff_dmaor);
    runtime.system_bus_control->write(system_bus_register::Channel2Length, 64u);
    runtime.system_bus_control->write(system_bus_register::Channel2Start, 1u);
    require(runtime.pvr_ta_fifo->metrics().packets == packets_after_success &&
                event_count(SystemAsicEvent::PvrIllegalAddress) ==
                    illegal_before_area3_end + 1u &&
                runtime.dmac->address_error() &&
                runtime.system_bus_control->read(system_bus_register::Channel2Length) == 0u &&
                runtime.system_bus_control->read(system_bus_register::Channel2Start) == 0u,
            "Channel 2 akzeptiert einen Transfer ueber das physische Area-3-Ende hinaus.");
    std::cout << "KR-4603 Dreamcast-System-ASIC und Interruptintegration erfolgreich.\n";
}
