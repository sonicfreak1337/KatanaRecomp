#include "katana/runtime/holly_dma.hpp"
#include "katana/runtime/dma.hpp"
#include "katana/runtime/pvr.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
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

class RejectSecondDmaUnitMemoryDevice final : public katana::runtime::MemoryDevice {
  public:
    [[nodiscard]] std::size_t size() const noexcept override { return bytes_.size(); }

    [[nodiscard]] std::uint8_t read_u8(const std::uint32_t offset) const override {
        if (offset >= bytes_.size()) throw std::out_of_range("DMA-Testdevice-Leseoffset.");
        return bytes_[offset];
    }

    void write_u8(const std::uint32_t offset, const std::uint8_t value) override {
        if (offset >= bytes_.size()) throw std::out_of_range("DMA-Testdevice-Schreiboffset.");
        if (offset >= 32u) throw std::runtime_error("Synthetischer DMA-Teilbatchfehler.");
        bytes_[offset] = value;
    }

  private:
    std::array<std::uint8_t, 64u> bytes_{};
};
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
    const auto g2_snapshot = controllers.g2->snapshot();
    require(g2_snapshot.ds_timeout == 0x1Bu && g2_snapshot.tr_timeout == 0x271u &&
                g2_snapshot.modem_timeout == 0u && g2_snapshot.modem_wait == 1u &&
                g2_snapshot.address_protect == 0x00007F00u &&
                g2_snapshot.reset_observer != 0u && g2_snapshot.completion_observer_bound,
            "G2-DMA-Snapshot verliert private Timeout-/Protectregister oder Eventbindung.");

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

    for (std::uint32_t index = 0u; index < 4096u; ++index)
        memory.write_u8(0x0C006000u + index, static_cast<std::uint8_t>(index));
    memory.write_u32(0x005F7840u, 0x00806000u);
    memory.write_u32(0x005F7844u, 0x0C006000u);
    memory.write_u32(0x005F7848u, 4096u);
    memory.write_u32(0x005F784Cu, 0u);
    memory.write_u32(0x005F7850u, 4u);
    memory.write_u32(0x005F7854u, 1u);
    memory.write_u32(0x005F7858u, 1u);
    static_cast<void>(scheduler.advance_by(8192u, 1u));
    require(memory.read_u32(0x005F7840u) == 0x00806000u &&
                memory.read_u32(0x005F7844u) == 0x0C006000u &&
                memory.read_u32(0x005F78E0u) == 0x00806800u &&
                memory.read_u32(0x005F78E4u) == 0x0C006800u &&
                memory.read_u32(0x005F78E8u) == 2048u &&
                memory.read_u8(0x008067FFu) == 0xFFu &&
                memory.read_u8(0x00806800u) == 0u,
            "G2-DMA meldet nach dem ersten Chunk keine getrennten Live-Zaehler oder Residue.");
    memory.write_u32(0x005F785Cu, 1u);
    const auto g2_suspended = controllers.g2->channel_state(2u);
    static_cast<void>(scheduler.advance_by(8192u, 1u));
    require(g2_suspended.remaining_cycles == 8192u &&
                controllers.g2->channel_state(2u).remaining == 2048u &&
                memory.read_u8(0x00806801u) == 0u,
            "G2-DMA-Suspend verliert Chunk-Restzyklen oder laesst Daten weiterlaufen.");
    memory.write_u32(0x005F785Cu, 0u);
    static_cast<void>(scheduler.advance_by(8192u, 1u));
    require(controllers.g2->channel_state(2u).active == 0u &&
                controllers.g2->channel_state(2u).remaining == 0u &&
                memory.read_u8(0x00806FFFu) == 0xFFu &&
                events.back() == SystemAsicEvent::Ext2Dma,
            "G2-DMA-Resume schliesst den verbliebenen Chunk nicht deterministisch ab.");

    for (std::uint32_t index = 0u; index < 4096u; ++index)
        memory.write_u8(0x0C007000u + index, static_cast<std::uint8_t>(0x80u + index));
    const auto g2_events_before_abort = events.size();
    memory.write_u32(0x005F7840u, 0x00807000u);
    memory.write_u32(0x005F7844u, 0x0C007000u);
    memory.write_u32(0x005F7848u, 4096u);
    memory.write_u32(0x005F7850u, 0u);
    memory.write_u32(0x005F7854u, 1u);
    memory.write_u32(0x005F7858u, 1u);
    static_cast<void>(scheduler.advance_by(8192u, 1u));
    memory.write_u32(0x005F7854u, 0u);
    const auto g2_aborted = controllers.g2->channel_state(2u);
    static_cast<void>(scheduler.advance_by(8192u, 1u));
    require(g2_aborted.active == 0u && g2_aborted.remaining == 2048u &&
                g2_aborted.peripheral_counter == 0x00807800u &&
                g2_aborted.system_counter == 0x0C007800u &&
                controllers.g2->channel_state(2u).remaining == 2048u &&
                memory.read_u8(0x00807800u) == 0u &&
                events.size() == g2_events_before_abort,
            "G2-DMA-Abort verwirft Residue oder fuehrt einen spaeten Chunk aus.");

    Memory g2_fault_memory(0u);
    g2_fault_memory.map_region(
        "fault-system", 0x0C100000u, std::make_shared<LinearMemoryDevice>(0x1000u));
    g2_fault_memory.map_region(
        "fault-g2", 0x00820000u, std::make_shared<RejectSecondDmaUnitMemoryDevice>());
    for (std::uint32_t index = 0u; index < 64u; ++index)
        g2_fault_memory.write_u8(
            0x0C100000u + index, static_cast<std::uint8_t>(index + 1u));
    EventScheduler g2_fault_scheduler;
    std::vector<SystemAsicEvent> g2_fault_events;
    DreamcastG2DmaController g2_fault(
        g2_fault_memory,
        g2_fault_scheduler,
        HollyDmaTiming{4u},
        [&](const auto event) { g2_fault_events.push_back(event); });
    g2_fault.write(0x00u, 0x00820000u);
    g2_fault.write(0x04u, 0x0C100000u);
    g2_fault.write(0x08u, 64u);
    g2_fault.write(0x0Cu, 0u);
    g2_fault.write(0x10u, 0u);
    g2_fault.write(0x14u, 1u);
    g2_fault.write(0x18u, 1u);
    static_cast<void>(g2_fault_scheduler.advance_by(256u, 1u));
    const auto g2_partial_fault = g2_fault.channel_state(0u);
    require(g2_fault.last_fault() &&
                g2_fault.last_fault()->reason == HollyDmaFaultReason::TransferFailure &&
                g2_fault.last_fault()->remaining == 32u &&
                g2_partial_fault.active == 0u &&
                g2_partial_fault.peripheral_counter == 0x00820020u &&
                g2_partial_fault.system_counter == 0x0C100020u &&
                g2_partial_fault.remaining == 32u &&
                g2_fault_memory.read_u8(0x0082001Fu) == 32u &&
                g2_fault_memory.read_u8(0x00820020u) == 0u &&
                g2_fault_events ==
                    std::vector<SystemAsicEvent>{SystemAsicEvent::AicaDmaIllegalAddress},
            "G2-DMA-Teilbatchfehler verliert den committed Prefix oder seine Livezaehler.");

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
                                                       SystemAsicEvent::Ext2Dma,
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
    const auto pvr_live_before_reverse = controllers.pvr->state();
    const auto pvr_events_before_reverse = events.size();
    memory.write_u8(0x0C003800u, 0x5Au);
    memory.write_u8(0x04002800u, 0xC3u);
    memory.write_u32(0x005F7C00u, 0x04002800u);
    memory.write_u32(0x005F7C04u, 0x0C003800u);
    memory.write_u32(0x005F7C08u, 32u);
    memory.write_u32(0x005F7C0Cu, 1u);
    memory.write_u32(0x005F7C10u, 0u);
    memory.write_u32(0x005F7C14u, 1u);
    memory.write_u32(0x005F7C18u, 1u);
    static_cast<void>(scheduler.advance_by(128u, 1u));
    require(controllers.pvr->last_fault() &&
                controllers.pvr->last_fault()->reason ==
                    HollyDmaFaultReason::InvalidDirection &&
                controllers.pvr->last_fault()->event ==
                    SystemAsicEvent::PvrIllegalAddress &&
                events.size() == pvr_events_before_reverse + 1u &&
                events.back() == SystemAsicEvent::PvrIllegalAddress &&
                controllers.pvr->state().peripheral_counter ==
                    pvr_live_before_reverse.peripheral_counter &&
                controllers.pvr->state().system_counter ==
                    pvr_live_before_reverse.system_counter &&
                memory.read_u8(0x0C003800u) == 0x5Au &&
                memory.read_u8(0x04002800u) == 0xC3u,
            "Reverse-PVR-DMA wird nicht typisiert und ohne Datenmutation abgewiesen.");
    memory.write_u32(0x005F7C00u, 0x04002000u);
    memory.write_u32(0x005F7C04u, 0u);
    memory.write_u32(0x005F7C08u, 32u);
    memory.write_u32(0x005F7C0Cu, 0u);
    memory.write_u32(0x005F7C10u, 0u);
    memory.write_u32(0x005F7C14u, 1u);
    memory.write_u32(0x005F7C18u, 1u);
    require(controllers.pvr->last_fault() &&
                controllers.pvr->last_fault()->reason == HollyDmaFaultReason::Overrun &&
                events.back() == SystemAsicEvent::PvrOverrun &&
                controllers.pvr->state().active == 0u,
            "Ungueltiger PVR-DMA-Bereich entkommt als Hostfehler statt als ASIC-Fault.");

    Memory pvr_fault_memory(0u);
    pvr_fault_memory.map_region(
        "fault-system", 0x0C100000u, std::make_shared<LinearMemoryDevice>(0x1000u));
    const auto pvr_fault_fifo = std::make_shared<PvrTaFifo>();
    const auto pvr_fault_aperture =
        std::make_shared<PvrTaFifoMemoryDevice>(pvr_fault_fifo);
    pvr_fault_memory.map_region("fault-ta", 0x10000000u, pvr_fault_aperture);
    pvr_fault_memory.write_u32(0x0C100000u, 0x80000000u);
    pvr_fault_memory.write_u32(0x0C100020u, 0x60000000u);
    EventScheduler pvr_fault_scheduler;
    std::vector<SystemAsicEvent> pvr_fault_events;
    DreamcastPvrDmaController pvr_fault(
        pvr_fault_memory,
        pvr_fault_scheduler,
        HollyDmaTiming{4u},
        [&](const auto event) { pvr_fault_events.push_back(event); });
    pvr_fault.write(0x00u, 0x10000000u);
    pvr_fault.write(0x04u, 0x0C100000u);
    pvr_fault.write(0x08u, 64u);
    pvr_fault.write(0x0Cu, 0u);
    pvr_fault.write(0x10u, 0u);
    pvr_fault.write(0x14u, 1u);
    pvr_fault.write(0x18u, 1u);
    static_cast<void>(pvr_fault_scheduler.advance_by(256u, 1u));
    const auto pvr_partial_fault = pvr_fault.state();
    require(pvr_fault.last_fault() &&
                pvr_fault.last_fault()->reason ==
                    HollyDmaFaultReason::TransferFailure &&
                pvr_fault.last_fault()->remaining == 32u &&
                pvr_partial_fault.active == 0u &&
                pvr_partial_fault.peripheral_counter == 0x10000020u &&
                pvr_partial_fault.system_counter == 0x0C100020u &&
                pvr_partial_fault.remaining == 32u &&
                pvr_fault_fifo->metrics().packets == 1u &&
                pvr_fault_fifo->metrics().rejected_packets == 1u &&
                !pvr_fault_aperture->snapshot().packet_active &&
                pvr_fault_events ==
                    std::vector<SystemAsicEvent>{SystemAsicEvent::PvrIllegalAddress},
            "PVR-DMA-TA-Teilbatchfehler verliert Prefix, Residue oder Parserquieszenz.");

    EventScheduler g1_scheduler;
    bool g1_bytes_committed = false;
    bool g1_completed = false;
    DreamcastG1BusController g1(
        g1_scheduler,
        HollyDmaTiming{4u},
        [&](const std::uint32_t address,
            const std::uint32_t length,
            const std::uint32_t direction) {
            require(address == 0x0C002000u && length == 32u && direction == 1u,
                    "G1-DMA verliert Transferparameter bis zur Completion.");
            g1_bytes_committed = true;
        },
        [&](const SystemAsicEvent event) { g1_completed = event == SystemAsicEvent::GdromDma; });
    g1.write(0x04u, 0x0C002000u);
    g1.write(0x08u, 32u);
    g1.write(0x0Cu, 1u);
    g1.write(0x14u, 1u);
    g1.write(0x18u, 1u);
    const auto g1_snapshot = g1.snapshot();
    require(!g1_bytes_committed && !g1_completed && g1.read(0x18u) == 1u &&
                g1.read(0x04u) == 0x0C002000u && g1.read(0x08u) == 32u &&
                g1.read(0xF4u) == 0x0C002000u && g1.read(0xF8u) == 0u,
            "G1-DMA macht Daten bereits beim Start sichtbar.");
    require(g1_snapshot.channel.completion_event.has_value() &&
                g1_snapshot.channel.remaining == 32u &&
                g1_snapshot.timing == HollyDmaTiming{4u} &&
                g1_snapshot.reset_observer != 0u &&
                g1_snapshot.transfer_handler_bound &&
                g1_snapshot.completion_observer_bound,
            "G1-DMA-Snapshot verliert Chunk-FSM, Timing oder Schedulerbindung.");
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
    g1.restore_bios_handoff();
    require(g1.begin_transfer(0x0C002000u, 32u, 1u),
            "Stale-Prefix-Regression kann keinen erfolgreichen Vortransfer starten.");
    static_cast<void>(g1_scheduler.advance_by(128u, 1u));
    std::optional<G1DmaFault> clean_start_fault;
    g1.set_fault_observer([&](const G1DmaFault& fault) { clean_start_fault = fault; });
    require(!g1.begin_transfer(0x0BFFFFE0u, 32u, 1u) && clean_start_fault &&
                clean_start_fault->fault_address == 0x0BFFFFE0u &&
                clean_start_fault->transferred_bytes == 0u &&
                clean_start_fault->residue == 32u &&
                clean_start_fault->phase == G1DmaFaultPhase::Start,
            "Neuer synchron abgewiesener G1-Start erbt den Prefix des Vortransfers.");

    EventScheduler chunk_scheduler;
    std::vector<std::uint32_t> chunk_addresses;
    std::vector<std::uint32_t> chunk_lengths;
    std::vector<std::uint32_t> chunk_directions;
    std::vector<SystemAsicEvent> chunk_events;
    DreamcastG1BusController chunked_g1(
        chunk_scheduler,
        HollyDmaTiming{4u},
        [&](const std::uint32_t address,
            const std::uint32_t length,
            const std::uint32_t direction) {
            chunk_addresses.push_back(address);
            chunk_lengths.push_back(length);
            chunk_directions.push_back(direction);
        },
        [&](const SystemAsicEvent event) { chunk_events.push_back(event); });
    require(chunked_g1.begin_transfer(0x0C006000u, 4128u, 1u) &&
                chunked_g1.read(0x04u) == 0x0C006000u && chunked_g1.read(0x08u) == 4128u &&
                chunked_g1.read(0x0Cu) == 1u && chunked_g1.state().remaining == 4128u &&
                chunked_g1.state().completion_cycle == 8192u,
            "Oeffentlicher G1-Start uebernimmt keinen exakten gueltigen Transfervertrag.");
    static_cast<void>(chunk_scheduler.advance_by(8191u, 4u));
    require(chunk_addresses.empty() && chunk_events.empty(),
            "G1-DMA committed den ersten Chunk vor dessen Gastzeit.");
    static_cast<void>(chunk_scheduler.advance_by(1u, 1u));
    require(chunk_addresses == std::vector<std::uint32_t>{0x0C006000u} &&
                chunk_lengths == std::vector<std::uint32_t>{2048u} &&
                chunk_directions == std::vector<std::uint32_t>{1u} &&
                chunked_g1.state().active == 1u &&
                chunked_g1.state().system_counter == 0x0C006800u &&
                chunked_g1.state().peripheral_counter == 2048u &&
                chunked_g1.state().remaining == 2080u && chunk_events.empty(),
            "Erster G1-Chunk aktualisiert Liveadresse, Transferzaehler oder Residue falsch.");
    static_cast<void>(chunk_scheduler.advance_by(8192u, 1u));
    require(chunk_addresses == std::vector<std::uint32_t>{0x0C006000u, 0x0C006800u} &&
                chunk_lengths == std::vector<std::uint32_t>{2048u, 2048u} &&
                chunked_g1.state().system_counter == 0x0C007000u &&
                chunked_g1.state().peripheral_counter == 4096u &&
                chunked_g1.state().remaining == 32u && chunk_events.empty(),
            "Zweiter G1-Chunk ist nicht separat gastzeitgebunden.");
    static_cast<void>(chunk_scheduler.advance_by(128u, 1u));
    require(
        chunk_addresses == std::vector<std::uint32_t>{0x0C006000u, 0x0C006800u, 0x0C007000u} &&
            chunk_lengths == std::vector<std::uint32_t>{2048u, 2048u, 32u} &&
            chunked_g1.state().active == 0u && chunked_g1.state().system_counter == 0x0C007020u &&
            chunked_g1.state().peripheral_counter == 4128u && chunked_g1.state().remaining == 0u &&
            chunk_events == std::vector<SystemAsicEvent>{SystemAsicEvent::GdromDma},
        "G1-DMA erzeugt nicht genau einen Abschluss nach dem letzten Chunk.");

    const auto committed_before_abort = chunk_addresses.size();
    require(chunked_g1.begin_transfer(0x0C008000u, 4096u, 1u),
            "Gueltiger G1-Transfer kann nach Completion nicht erneut starten.");
    static_cast<void>(chunk_scheduler.advance_by(8192u, 1u));
    chunked_g1.abort_transfer();
    require(chunked_g1.state().active == 0u && !chunked_g1.state().completion_event &&
                chunked_g1.state().remaining == 0u &&
                chunk_addresses.size() == committed_before_abort + 1u,
            "G1-Abort beendet den aktiven Transfer nicht am Chunk-Safepoint.");
    static_cast<void>(chunk_scheduler.advance_by(16384u, 16u));
    require(chunk_addresses.size() == committed_before_abort + 1u &&
                chunk_events == std::vector<SystemAsicEvent>{SystemAsicEvent::GdromDma},
            "G1-Abort laesst einen spaeten Chunk oder Completion-Interrupt durch.");

    require(chunked_g1.begin_transfer(0x0C00A000u, 4096u, 1u),
            "G1-Reset-Test kann Transfer nicht starten.");
    chunked_g1.reset();
    static_cast<void>(chunk_scheduler.advance_by(16384u, 16u));
    require(chunk_addresses.size() == committed_before_abort + 1u &&
                chunk_events == std::vector<SystemAsicEvent>{SystemAsicEvent::GdromDma} &&
                chunked_g1.state().active == 0u && !chunked_g1.state().completion_event,
            "G1-Reset laesst einen spaeten Chunk oder Completion-Interrupt durch.");

    require(chunked_g1.begin_transfer(0x0C00C000u, 4096u, 1u),
            "G1-Scheduler-Reset-Test kann Transfer nicht starten.");
    chunk_scheduler.reset();
    static_cast<void>(chunk_scheduler.advance_by(16384u, 16u));
    require(chunk_addresses.size() == committed_before_abort + 1u &&
                chunk_events == std::vector<SystemAsicEvent>{SystemAsicEvent::GdromDma} &&
                chunked_g1.state().active == 0u && !chunked_g1.state().completion_event,
            "Scheduler-Reset laesst einen spaeten G1-Chunk oder Completion-Interrupt durch.");

    require(!chunked_g1.begin_transfer(0x0C009001u, 32u, 1u) && chunked_g1.last_fault() &&
                chunked_g1.last_fault()->reason == HollyDmaFaultReason::IllegalAddress,
            "G1-DMA akzeptiert eine nicht auf 32 Byte ausgerichtete Zieladresse.");
    chunked_g1.reset();
    require(!chunked_g1.begin_transfer(0x0C009000u, 33u, 1u) && chunked_g1.last_fault() &&
                chunked_g1.last_fault()->reason == HollyDmaFaultReason::InvalidLength,
            "G1-DMA akzeptiert eine nicht auf 32 Byte ausgerichtete Laenge.");
    chunked_g1.reset();
    require(!chunked_g1.begin_transfer(0x0C009000u, 32u, 0u) && chunked_g1.last_fault() &&
                chunked_g1.last_fault()->reason == HollyDmaFaultReason::InvalidDirection,
            "G1-DMA akzeptiert die falsche System-zu-Laufwerk-Richtung.");
    chunked_g1.reset();
    require(chunked_g1.begin_transfer(0x0C009000u, 64u, 1u) &&
                !chunked_g1.begin_transfer(0x0C00A000u, 32u, 1u) && chunked_g1.last_fault() &&
                chunked_g1.last_fault()->reason == HollyDmaFaultReason::Overrun &&
                chunked_g1.last_fault()->event == SystemAsicEvent::GdromOverrun &&
                chunk_events.back() == SystemAsicEvent::GdromOverrun,
            "G1-DMA akzeptiert einen zweiten Start waehrend eines aktiven Transfers.");
    static_cast<void>(chunk_scheduler.advance_by(1024u, 16u));
    require(chunk_addresses.size() == committed_before_abort + 1u,
            "Abgewiesener G1-Overrun laesst den alten Transfer spaet weiterlaufen.");

    EventScheduler protected_g1_scheduler;
    std::vector<SystemAsicEvent> protected_g1_events;
    DreamcastG1BusController protected_g1(
        protected_g1_scheduler,
        HollyDmaTiming{4u},
        [](const auto, const auto, const auto) {},
        [&](const SystemAsicEvent event) { protected_g1_events.push_back(event); });
    require(protected_g1.address_protect() == 0x0000407Fu,
            "G1-GDAPRO startet nicht mit dem Post-BIOS-Hauptspeicherfenster.");
    protected_g1.write(0xA0u, 0x12345678u);
    require(protected_g1.gdrom_read_access_timing() == 0x12345678u &&
                throws([&] { static_cast<void>(protected_g1.read(0xA0u)); }),
            "G1GDRC wird nicht als write-only Diagnostikwert gespeichert.");
    protected_g1.write(0xB8u, 0x88434040u);
    protected_g1.write(0xB8u, 0x12344141u);
    require(protected_g1.address_protect() == 0x00004040u &&
                throws([&] { static_cast<void>(protected_g1.read(0xB8u)); }),
            "GDAPRO akzeptiert einen falschen Schluessel oder ist gastseitig lesbar.");
    require(protected_g1.begin_transfer(0x0C000000u, 32u, 1u),
            "GDAPRO lehnt die exakte untere Schutzgrenze ab.");
    protected_g1.abort_transfer();
    require(protected_g1.begin_transfer(0x0C0FFFE0u, 32u, 1u),
            "GDAPRO lehnt die exakte obere Schutzgrenze ab.");
    protected_g1.abort_transfer();
    require(!protected_g1.begin_transfer(0x0BFFFFE0u, 32u, 1u) &&
                protected_g1.last_fault() &&
                protected_g1.last_fault()->reason == HollyDmaFaultReason::IllegalAddress &&
                protected_g1.last_fault()->event == SystemAsicEvent::GdromIllegalAddress,
            "GDAPRO akzeptiert einen Bereich unterhalb der Schutzgrenze.");
    protected_g1.reset();
    protected_g1.write(0xB8u, 0x88434040u);
    require(!protected_g1.begin_transfer(0x0C0FFFE0u, 64u, 1u) &&
                protected_g1.last_fault() &&
                protected_g1.last_fault()->reason == HollyDmaFaultReason::IllegalAddress,
            "G1-DMA prueft nur den ersten Chunk statt der vollstaendigen Zielspanne.");
    protected_g1.reset();
    protected_g1.write(0xB8u, 0x88434140u);
    require(!protected_g1.begin_transfer(0x0C100000u, 32u, 1u) &&
                protected_g1.last_fault() &&
                protected_g1.last_fault()->reason == HollyDmaFaultReason::IllegalAddress,
            "GDAPRO akzeptiert ein invertiertes Schutzfenster.");
    protected_g1.reset();
    require(!protected_g1.begin_transfer(0xFFFFFFE0u, 64u, 1u) &&
                protected_g1.last_fault() &&
                protected_g1.last_fault()->reason == HollyDmaFaultReason::IllegalAddress,
            "G1-DMA akzeptiert einen 32-Bit-ueberlaufenden Zielbereich.");
    protected_g1.reset();
    require(protected_g1.begin_transfer(0x0C000000u, 0u, 1u) &&
                protected_g1.read(0x08u) == 0u &&
                protected_g1.state().remaining ==
                    DreamcastG1BusController::maximum_transfer_bytes,
            "GDLEN=0 wird nicht als 32-MiB-Hardwarekodierung dekodiert.");
    protected_g1.abort_transfer();
    require(!protected_g1.begin_transfer(0x0F000000u, 0u, 1u) &&
                protected_g1.last_fault() &&
                protected_g1.last_fault()->reason == HollyDmaFaultReason::IllegalAddress,
            "Dekodiertes GDLEN=0 entkommt der Vollbereichspruefung.");

    Memory bounded_g1_memory(0u);
    bounded_g1_memory.map_region(
        "bounded-system", 0x0C000000u, std::make_shared<LinearMemoryDevice>(0x1000u));
    EventScheduler bounded_g1_scheduler;
    std::uint64_t bounded_g1_handler_bytes = 0u;
    std::vector<SystemAsicEvent> bounded_g1_events;
    const auto bounded_g1 = map_dreamcast_holly_dma(
        bounded_g1_memory,
        bounded_g1_scheduler,
        HollyDmaTiming{4u},
        [&](const SystemAsicEvent event) { bounded_g1_events.push_back(event); },
        [&](const auto, const auto length, const auto) { bounded_g1_handler_bytes += length; });
    bounded_g1.g1->write(0xB8u, 0x88434040u);
    require(bounded_g1.g1->begin_transfer(0x0C000FE0u, 32u, 1u),
            "G1-Produktpfad lehnt das exakte Ende eines schreibbaren linearen Mappings ab.");
    bounded_g1.g1->abort_transfer();
    require(!bounded_g1.g1->begin_transfer(0x0C000FE0u, 64u, 1u) &&
                bounded_g1_handler_bytes == 0u && bounded_g1.g1->last_fault() &&
                bounded_g1.g1->last_fault()->reason == HollyDmaFaultReason::IllegalAddress &&
                bounded_g1_events ==
                    std::vector<SystemAsicEvent>{SystemAsicEvent::GdromIllegalAddress},
            "G1-Produktpfad startet einen Chunk ueber das reale Zielmapping hinaus.");

    EventScheduler failing_g1_scheduler;
    std::vector<SystemAsicEvent> failing_g1_events;
    DreamcastG1BusController failing_g1(
        failing_g1_scheduler,
        HollyDmaTiming{4u},
        [](const auto, const auto, const auto) { throw std::runtime_error("synthetic"); },
        [&](const SystemAsicEvent event) { failing_g1_events.push_back(event); });
    std::optional<G1DmaFault> observed_g1_fault;
    bool g1_quiesced_before_notification = false;
    failing_g1.set_fault_observer([&](const G1DmaFault& fault) {
        observed_g1_fault = fault;
        const auto state = failing_g1.state();
        g1_quiesced_before_notification =
            state.active == 0u && state.enabled == 0u && !state.completion_event;
        throw std::runtime_error("synthetic fault observer");
    });
    require(failing_g1.begin_transfer(0x0C000000u, 32u, 1u),
            "G1-Handlerfehler-Test kann den Transfer nicht starten.");
    static_cast<void>(failing_g1_scheduler.advance_by(128u, 1u));
    require(failing_g1.last_fault() && failing_g1.last_g1_fault() && observed_g1_fault &&
                failing_g1.last_fault()->reason == HollyDmaFaultReason::TransferFailure &&
                !failing_g1.last_fault()->event && failing_g1_events.empty() &&
                observed_g1_fault->reason == HollyDmaFaultReason::TransferFailure &&
                observed_g1_fault->fault_address == 0x0C000000u &&
                observed_g1_fault->transferred_bytes == 0u &&
                observed_g1_fault->residue == 32u &&
                observed_g1_fault->phase == G1DmaFaultPhase::Chunk &&
                *observed_g1_fault == *failing_g1.last_g1_fault() &&
                g1_quiesced_before_notification,
            "Interner G1-Handlerfehler verliert typisierte Evidenz, Quieszenz oder wird als "
            "Hardware-ASIC-Ereignis ausgegeben.");
    static_cast<void>(failing_g1_scheduler.advance_by(1'024u, 16u));
    require(failing_g1.state().active == 0u && !failing_g1.state().completion_event &&
                failing_g1.state().fault_count == 1u,
            "Werfender G1-Faultobserver laesst ein spaetes Event oder einen zweiten Fault durch.");

    EventScheduler scheduler_failure_g1_scheduler;
    std::vector<SystemAsicEvent> scheduler_failure_g1_events;
    DreamcastG1BusController scheduler_failure_g1(
        scheduler_failure_g1_scheduler,
        HollyDmaTiming{std::numeric_limits<std::uint64_t>::max()},
        [](const auto, const auto, const auto) {},
        [&](const SystemAsicEvent event) { scheduler_failure_g1_events.push_back(event); });
    require(!scheduler_failure_g1.begin_transfer(0x0C000000u, 32u, 1u) &&
                scheduler_failure_g1.last_fault() &&
                scheduler_failure_g1.last_fault()->reason ==
                    HollyDmaFaultReason::SchedulerFailure &&
                !scheduler_failure_g1.last_fault()->event &&
                scheduler_failure_g1_events.empty(),
            "Interner G1-Schedulerfehler wird als Timeout/Overrun auf das ASIC gespiegelt.");

    EventScheduler scheduler_failure_g2_scheduler;
    std::vector<SystemAsicEvent> scheduler_failure_g2_events;
    DreamcastG2DmaController scheduler_failure_g2(
        memory,
        scheduler_failure_g2_scheduler,
        HollyDmaTiming{std::numeric_limits<std::uint64_t>::max()},
        [&](const SystemAsicEvent event) {
            scheduler_failure_g2_events.push_back(event);
        });
    scheduler_failure_g2.write(0x00u, 0x00800000u);
    scheduler_failure_g2.write(0x04u, 0x0C000000u);
    scheduler_failure_g2.write(0x08u, 32u);
    scheduler_failure_g2.write(0x0Cu, 0u);
    scheduler_failure_g2.write(0x10u, 0u);
    scheduler_failure_g2.write(0x14u, 1u);
    scheduler_failure_g2.write(0x18u, 1u);
    require(scheduler_failure_g2.last_fault() &&
                scheduler_failure_g2.last_fault()->reason ==
                    HollyDmaFaultReason::SchedulerFailure &&
                !scheduler_failure_g2.last_fault()->event &&
                scheduler_failure_g2_events.empty() &&
                scheduler_failure_g2.channel_state(0u).active == 0u &&
                scheduler_failure_g2.channel_state(0u).enabled == 0u,
            "Interner G2-Schedulerfehler wird als Hardware-Timeout/Overrun ausgegeben.");

    EventScheduler scheduler_failure_pvr_scheduler;
    std::vector<SystemAsicEvent> scheduler_failure_pvr_events;
    DreamcastPvrDmaController scheduler_failure_pvr(
        memory,
        scheduler_failure_pvr_scheduler,
        HollyDmaTiming{std::numeric_limits<std::uint64_t>::max()},
        [&](const SystemAsicEvent event) {
            scheduler_failure_pvr_events.push_back(event);
        });
    scheduler_failure_pvr.write(0x00u, 0x04000000u);
    scheduler_failure_pvr.write(0x04u, 0x0C000000u);
    scheduler_failure_pvr.write(0x08u, 32u);
    scheduler_failure_pvr.write(0x0Cu, 0u);
    scheduler_failure_pvr.write(0x10u, 0u);
    scheduler_failure_pvr.write(0x14u, 1u);
    scheduler_failure_pvr.write(0x18u, 1u);
    require(scheduler_failure_pvr.last_fault() &&
                scheduler_failure_pvr.last_fault()->reason ==
                    HollyDmaFaultReason::SchedulerFailure &&
                !scheduler_failure_pvr.last_fault()->event &&
                scheduler_failure_pvr_events.empty() &&
                scheduler_failure_pvr.state().active == 0u &&
                scheduler_failure_pvr.state().enabled == 0u,
            "Interner PVR-Schedulerfehler wird als Hardware-Timeout/Overrun ausgegeben.");

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
    const auto bound_pvr_snapshot = contract_pvr.snapshot();
    require(bound_pvr_snapshot.dmac_bound &&
                bound_pvr_snapshot.dmac_contract_required &&
                bound_pvr_snapshot.dmac_channel == 0u &&
                bound_pvr_snapshot.reset_observer != 0u,
            "PVR-DMA-Snapshot verliert SH-4-DMAC-Vertrag oder Eventbindung.");
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

    contract_dmac->reset();
    contract_pvr.reset();
    contract_events.clear();
    for (std::uint32_t index = 0u; index < 4096u; ++index)
        memory.write_u8(0x0C008000u + index, static_cast<std::uint8_t>(index));
    contract_dmac->write_source(0u, 0x0C008000u);
    contract_dmac->write_count(0u, 128u);
    contract_dmac->write_control(0u, 0x00001841u);
    contract_dmac->write_operation(Sh4Dmac::master_enable);
    contract_pvr.write(0x00u, 0x04008000u);
    contract_pvr.write(0x04u, 0x0C008000u);
    contract_pvr.write(0x08u, 4096u);
    contract_pvr.write(0x14u, 1u);
    contract_pvr.write(0x18u, 1u);
    static_cast<void>(contract_scheduler.advance_by(8192u, 1u));
    const auto pvr_partial = contract_pvr.state();
    require(pvr_partial.active == 1u && pvr_partial.remaining == 2048u &&
                pvr_partial.peripheral_counter == 0x04008800u &&
                pvr_partial.system_counter == 0x0C008800u &&
                contract_dmac->source(0u) == 0x0C008800u &&
                contract_dmac->count(0u) == 64u &&
                (contract_dmac->control(0u) & Sh4Dmac::transfer_end) == 0u &&
                memory.read_u8(0x040087FFu) == 0xFFu &&
                memory.read_u8(0x04008800u) == 0u,
            "PVR-DMA-Teilfortschritt erreicht Live-Zaehler oder SH-4-DMAC-Residue nicht.");
    contract_pvr.set_suspended(true);
    const auto pvr_suspended = contract_pvr.state();
    static_cast<void>(contract_scheduler.advance_by(8192u, 1u));
    require(pvr_suspended.remaining_cycles == 8192u &&
                contract_pvr.state().remaining == 2048u &&
                contract_dmac->count(0u) == 64u &&
                memory.read_u8(0x04008801u) == 0u,
            "PVR-DMA-Suspend verliert Restzyklen oder committed den naechsten Chunk.");
    contract_pvr.set_suspended(false);
    static_cast<void>(contract_scheduler.advance_by(8192u, 1u));
    require(contract_pvr.state().active == 0u && contract_pvr.state().remaining == 0u &&
                contract_dmac->source(0u) == 0x0C009000u &&
                contract_dmac->count(0u) == 0u &&
                (contract_dmac->control(0u) & Sh4Dmac::transfer_end) != 0u &&
                memory.read_u8(0x04008FFFu) == 0xFFu &&
                contract_events == std::vector<SystemAsicEvent>{SystemAsicEvent::PvrDma},
            "PVR-DMA-Resume finalisiert Daten und DMAC-Vertrag nicht gemeinsam.");

    contract_dmac->reset();
    contract_pvr.reset();
    contract_events.clear();
    for (std::uint32_t index = 0u; index < 4096u; ++index)
        memory.write_u8(0x0C00A000u + index, static_cast<std::uint8_t>(0x40u + index));
    contract_dmac->write_source(0u, 0x0C00A000u);
    contract_dmac->write_count(0u, 128u);
    contract_dmac->write_control(0u, 0x00001841u);
    contract_dmac->write_operation(Sh4Dmac::master_enable);
    contract_pvr.write(0x00u, 0x0400A000u);
    contract_pvr.write(0x04u, 0x0C00A000u);
    contract_pvr.write(0x08u, 4096u);
    contract_pvr.write(0x14u, 1u);
    contract_pvr.write(0x18u, 1u);
    static_cast<void>(contract_scheduler.advance_by(8192u, 1u));
    contract_pvr.abort();
    const auto pvr_aborted = contract_pvr.state();
    static_cast<void>(contract_scheduler.advance_by(8192u, 1u));
    require(pvr_aborted.active == 0u && pvr_aborted.enabled == 0u &&
                pvr_aborted.remaining == 2048u &&
                pvr_aborted.peripheral_counter == 0x0400A800u &&
                pvr_aborted.system_counter == 0x0C00A800u &&
                contract_pvr.state().remaining == 2048u &&
                contract_dmac->source(0u) == 0x0C00A800u &&
                contract_dmac->count(0u) == 64u &&
                (contract_dmac->control(0u) & Sh4Dmac::transfer_end) == 0u &&
                memory.read_u8(0x0400A800u) == 0u && contract_events.empty(),
            "PVR-DMA-Abort verwirft Residue, finalisiert den DMAC oder laesst einen spaeten Chunk laufen.");

    const auto events_before_missing_backend = events.size();
    memory.write_u32(0x005F7404u, 0x0C000000u);
    memory.write_u32(0x005F7408u, 32u);
    memory.write_u32(0x005F740Cu, 1u);
    memory.write_u32(0x005F7418u, 0u);
    memory.write_u32(0x005F7414u, 1u);
    memory.write_u32(0x005F7418u, 1u);
    require(controllers.g1->last_fault() &&
                controllers.g1->last_fault()->reason == HollyDmaFaultReason::MissingBackend &&
                !controllers.g1->last_fault()->event &&
                events.size() == events_before_missing_backend &&
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
