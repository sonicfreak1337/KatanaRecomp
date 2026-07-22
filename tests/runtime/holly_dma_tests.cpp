#include "katana/runtime/holly_dma.hpp"
#include "katana/runtime/dma.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
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
    Memory memory(0u);
    memory.map_region("system", 0x0C000000u, std::make_shared<LinearMemoryDevice>(1u << 20u));
    memory.map_region("g2", 0x00800000u, std::make_shared<LinearMemoryDevice>(2u << 20u));
    memory.map_region("pvr", 0x04000000u, std::make_shared<LinearMemoryDevice>(1u << 20u));
    EventScheduler scheduler;
    std::vector<SystemAsicEvent> events;
    const auto controllers = map_dreamcast_holly_dma(
        memory, scheduler, HollyDmaTiming{4u}, [&](const auto event) { events.push_back(event); });

    for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
        const auto base = 0xA05F7800u + channel * 0x20u;
        memory.write_u32(base, 0x009F0000u);
        memory.write_u32(base + 4u, 0x0CFF0000u);
        memory.write_u32(base + 8u, 0x20u);
        memory.write_u32(base + 0x0Cu, 0u);
        memory.write_u32(base + 0x10u, 5u);
        memory.write_u32(base + 0x14u, 0u);
        memory.write_u32(base + 0x1Cu, 0u);
    }
    memory.write_u32(0xA05F7890u, 0x1Bu);
    memory.write_u32(0xA05F7894u, 0x271u);
    memory.write_u32(0xA05F7898u, 0u);
    memory.write_u32(0xA05F789Cu, 1u);
    for (std::uint32_t offset = 0xA0u; offset <= 0xB8u; offset += 4u)
        memory.write_u32(0xA05F7800u + offset, 0u);
    memory.write_u32(0xA05F78BCu, 0x46597F00u);
    require(memory.read_u32(0x005F7800u) == 0x009F0000u &&
                memory.read_u32(0x805F7804u) == 0x0CFF0000u &&
                memory.read_u32(0x005F7808u) == 0x20u && memory.read_u32(0x005F7810u) == 5u &&
                memory.read_u32(0x005F7880u) == 0x12u && memory.read_u32(0x005F7894u) == 0x271u,
            "PAL-G2-Initialisierungswerte oder Direktsegmente werden nicht gespiegelt.");

    for (std::uint32_t index = 0u; index < 32u; ++index)
        memory.write_u8(0x0C001000u + index, static_cast<std::uint8_t>(index + 1u));
    memory.write_u32(0x005F7800u, 0x00801000u);
    memory.write_u32(0x005F7804u, 0x0C001000u);
    memory.write_u32(0x005F7808u, 0x80000020u);
    memory.write_u32(0x005F7814u, 1u);
    memory.write_u32(0x005F7818u, 1u);
    require(memory.read_u32(0x005F7818u) == 1u && memory.read_u8(0x0080101Fu) == 0u,
            "Hardwaregetriggerte AICA-G2-DMA startet vor dem AICA-Request.");
    controllers.g2->hardware_trigger(0u);
    memory.write_u32(0x005F781Cu, 1u);
    static_cast<void>(scheduler.advance_by(128u, 1u));
    require(memory.read_u8(0x0080101Fu) == 0u && (memory.read_u32(0x005F781Cu) & 0x10u) != 0u,
            "Suspend haelt den laufenden G2-DMA-Transfer nicht an.");
    memory.write_u32(0x005F781Cu, 0u);
    static_cast<void>(scheduler.advance_by(128u, 1u));
    require(memory.read_u32(0x005F7818u) == 0u && memory.read_u32(0x005F7814u) == 0u &&
                memory.read_u8(0x0080101Fu) == 32u && memory.read_u32(0x005F78C0u) == 0x00801020u &&
                memory.read_u32(0x005F78C4u) == 0x0C001020u && memory.read_u32(0x005F78C8u) == 0u &&
                events == std::vector<SystemAsicEvent>{SystemAsicEvent::AicaDma},
            "AICA-G2-DMA-Resume/Completion aktualisiert Daten, Status oder ASIC-Ereignis falsch.");

    for (std::uint32_t index = 0u; index < 32u; ++index)
        memory.write_u8(0x0C002000u + index, static_cast<std::uint8_t>(0x80u + index));
    memory.write_u32(0xA05F7C00u, 0x0400101Fu);
    memory.write_u32(0xA05F7C04u, 0x0C00201Fu);
    memory.write_u32(0xA05F7C08u, 0x3Fu);
    memory.write_u32(0xA05F7C0Cu, 0u);
    memory.write_u32(0xA05F7C10u, 0u);
    memory.write_u32(0xA05F7C14u, 1u);
    memory.write_u32(0xA05F7C18u, 1u);
    require(memory.read_u32(0x005F7C00u) == 0x04001000u &&
                memory.read_u32(0x805F7C04u) == 0x0C002000u &&
                memory.read_u32(0x005F7C08u) == 0x20u &&
                memory.read_u32(0x005F7C18u) == 1u && memory.read_u8(0x0400101Fu) == 0u,
            "PVR-DMA maskiert Register falsch oder committed vor der Gastzeit.");
    static_cast<void>(scheduler.advance_by(127u, 1u));
    require(memory.read_u8(0x0400101Fu) == 0u,
            "PVR-DMA committed Daten vor dem faelligen Schedulerzyklus.");
    static_cast<void>(scheduler.advance_by(1u, 1u));
    require(memory.read_u8(0x0400101Fu) == 0x9Fu && memory.read_u32(0x005F7C18u) == 0u &&
                memory.read_u32(0x005F7CF0u) == 0x04001020u &&
                memory.read_u32(0x005F7CF4u) == 0x0C002020u &&
                memory.read_u32(0x005F7CF8u) == 0u &&
                events == std::vector<SystemAsicEvent>{SystemAsicEvent::AicaDma,
                                                       SystemAsicEvent::PvrDma},
            "PVR-DMA-Completion aktualisiert Daten, Zaehler oder ASIC-Ereignis falsch.");

    for (std::uint32_t index = 0u; index < 32u; ++index)
        memory.write_u8(0x0C003000u + index, static_cast<std::uint8_t>(0x40u + index));
    memory.write_u32(0x005F7C00u, 0x04002000u);
    memory.write_u32(0x005F7C04u, 0x0C003000u);
    memory.write_u32(0x005F7C08u, 0x20u);
    memory.write_u32(0x005F7C10u, 1u);
    memory.write_u32(0x005F7C18u, 1u);
    require(memory.read_u32(0x005F7C18u) == 0u,
            "Hardwaregetriggerte PVR-DMA startet durch einen Softwarewrite.");
    controllers.pvr->hardware_trigger();
    static_cast<void>(scheduler.advance_by(128u, 1u));
    require(memory.read_u8(0x0400201Fu) == 0x5Fu && memory.read_u32(0x005F7C14u) == 1u,
            "Hardwaregetriggerte PVR-DMA behaelt Enable oder Daten nicht korrekt.");
    memory.write_u32(0x005F7C00u, 0x04002000u);
    memory.write_u32(0x005F7C04u, 0u);
    memory.write_u32(0x005F7C08u, 32u);
    memory.write_u32(0x005F7C10u, 0u);
    memory.write_u32(0x005F7C14u, 1u);
    memory.write_u32(0x005F7C18u, 1u);
    require(controllers.pvr->last_fault() &&
                controllers.pvr->last_fault()->reason == HollyDmaFaultReason::Overrun &&
                events.back() == SystemAsicEvent::PvrOverrun &&
                controllers.pvr->state().active == 0u,
            "Ungueltiger PVR-DMA-Bereich entkommt als Hostfehler statt als ASIC-Fault.");

    EventScheduler g1_scheduler;
    bool g1_bytes_committed = false;
    bool g1_completed = false;
    DreamcastG1BusController g1(
        g1_scheduler,
        HollyDmaTiming{4u},
        [&](const std::uint32_t address, const std::uint32_t length, const std::uint32_t direction) {
            require(address == 0x0C002000u && length == 32u && direction == 0u,
                    "G1-DMA verliert Transferparameter bis zur Completion.");
            g1_bytes_committed = true;
        },
        [&](const SystemAsicEvent event) {
            g1_completed = event == SystemAsicEvent::GdromDma;
        });
    g1.write(0x04u, 0x0C002000u);
    g1.write(0x08u, 32u);
    g1.write(0x0Cu, 0u);
    g1.write(0x14u, 1u);
    g1.write(0x18u, 1u);
    require(!g1_bytes_committed && !g1_completed && g1.read(0x18u) == 1u &&
                g1.read(0x04u) == 0x0C002000u && g1.read(0x08u) == 32u &&
                g1.read(0xF4u) == 0x0C002000u && g1.read(0xF8u) == 0u,
            "G1-DMA macht Daten bereits beim Start sichtbar.");
    static_cast<void>(g1_scheduler.advance_by(127u, 1u));
    require(!g1_bytes_committed, "G1-DMA committed Daten vor dem faelligen Schedulerzyklus.");
    static_cast<void>(g1_scheduler.advance_by(1u, 1u));
    require(g1_bytes_committed && g1_completed && g1.read(0x18u) == 0u &&
                g1.read(0x04u) == 0x0C002000u && g1.read(0x08u) == 32u &&
                g1.read(0xF4u) == 0x0C002020u && g1.read(0xF8u) == 32u &&
                g1.state().remaining == 0u,
            "G1-DMA trennt konfigurierte Register und Livezaehler nicht atomar.");
    g1.configure_bios_handoff(0x0C123456u);
    require(g1.read(0x04u) == 0x0C002000u && g1.read(0x08u) == 32u &&
                g1.read(0xF4u) == 0x0C123440u && g1.read(0xF8u) == 0u,
            "G1-BIOS-Handoff ueberschreibt konfigurierte DMA-Register.");

    memory.write_u32(0x005F7820u, 0x01000000u);
    memory.write_u32(0x005F7824u, 0x0C005000u);
    memory.write_u32(0x005F7828u, 32u);
    memory.write_u32(0x005F7830u, 0u);
    memory.write_u32(0x005F7834u, 1u);
    memory.write_u32(0x005F7838u, 1u);
    require(controllers.g2->last_fault() &&
                controllers.g2->last_fault()->reason == HollyDmaFaultReason::IllegalAddress &&
                events.back() == SystemAsicEvent::Ext1DmaIllegalAddress &&
                controllers.g2->channel_state(1u).active == 0u &&
                controllers.g2->channel_state(1u).enabled == 0u,
            "Ungueltige G2-DMA-Adresse entkommt als Hostfehler statt als ASIC-Fault.");

    std::vector<SystemAsicEvent> contract_events;
    EventScheduler contract_scheduler;
    auto contract_dmac = std::make_shared<Sh4Dmac>(
        contract_scheduler, memory, DmaTiming{1u}, DmaExecutionMode::DeterministicBatch);
    DreamcastPvrDmaController contract_pvr(
        memory,
        contract_scheduler,
        HollyDmaTiming{4u},
        [&](const SystemAsicEvent event) { contract_events.push_back(event); });
    contract_pvr.bind_sh4_dmac(contract_dmac, 0u);
    contract_dmac->write_source(0u, 0x0C004000u);
    contract_dmac->write_count(0u, 1u);
    contract_dmac->write_control(0u, 0x00001841u);
    contract_dmac->write_operation(Sh4Dmac::master_enable);
    contract_pvr.write(0x00u, 0x04003000u);
    contract_pvr.write(0x04u, 0x0C004000u);
    contract_pvr.write(0x08u, 64u);
    contract_pvr.write(0x14u, 1u);
    contract_pvr.write(0x18u, 1u);
    require(contract_pvr.last_fault() &&
                contract_pvr.last_fault()->reason == HollyDmaFaultReason::HandshakeMismatch &&
                contract_dmac->address_error() &&
                contract_events == std::vector<SystemAsicEvent>{SystemAsicEvent::PvrOverrun},
            "PVR-DMA akzeptiert eine SH-4-DMAC-Restlaenge, die nicht zum Transfer passt.");

    contract_dmac->reset();
    contract_pvr.reset();
    contract_events.clear();
    for (std::uint32_t index = 0u; index < 64u; ++index)
        memory.write_u8(0x0C004000u + index, static_cast<std::uint8_t>(0x20u + index));
    contract_dmac->write_source(0u, 0x0C004000u);
    contract_dmac->write_count(0u, 2u);
    contract_dmac->write_control(0u, 0x00001841u);
    contract_dmac->write_operation(Sh4Dmac::master_enable);
    contract_pvr.write(0x00u, 0x04003000u);
    contract_pvr.write(0x04u, 0x0C004000u);
    contract_pvr.write(0x08u, 64u);
    contract_pvr.write(0x14u, 1u);
    contract_pvr.write(0x18u, 1u);
    static_cast<void>(contract_scheduler.advance_by(256u, 1u));
    require(memory.read_u8(0x0400303Fu) == 0x5Fu &&
                contract_dmac->source(0u) == 0x0C004040u && contract_dmac->count(0u) == 0u &&
                (contract_dmac->control(0u) & Sh4Dmac::transfer_end) != 0u &&
                contract_events == std::vector<SystemAsicEvent>{SystemAsicEvent::PvrDma},
            "PVR-DMA committed Daten, SH-4-DMAC-Residue oder Completion nicht gemeinsam.");

    memory.write_u32(0x005F7418u, 0u);
    memory.write_u32(0x005F7414u, 1u);
    memory.write_u32(0x005F7418u, 1u);
    require(controllers.g1->last_fault() &&
                controllers.g1->last_fault()->reason == HollyDmaFaultReason::MissingBackend &&
                events.back() == SystemAsicEvent::GdromAccessError &&
                throws([&] { static_cast<void>(memory.read_u32(0x005F7400u)); }) &&
                throws([&] { static_cast<void>(memory.read_u16(0x005F7800u)); }) &&
                throws([&] { memory.write_u32(0x005F7880u, 1u); }) &&
                throws([&] { memory.write_u32(0x005F78A0u, 1u); }) &&
                throws([&] { static_cast<void>(memory.read_u16(0x005F7C00u)); }) &&
                throws([&] { static_cast<void>(memory.read_u32(0x005F7C80u)); }) &&
                throws([&] { memory.write_u32(0x005F7CF0u, 1u); }),
            "G1-/G2-/PVR-DMA-Gates, reservierte Register oder Breitenvertrag sind offen.");

    std::cout << "Dreamcast-G1/G2/PVR-DMA-Register und echte DMA-Pfade erfolgreich.\n";
}
