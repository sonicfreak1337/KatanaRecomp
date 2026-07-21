#include "katana/runtime/dma.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function> bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

constexpr std::uint32_t auto_longword_increment =
    0x00004000u | 0x00001000u | 0x00000400u | 0x00000030u;

} // namespace

int main() {
    using namespace katana::runtime;

    EventScheduler scheduler;
    Memory memory(512u, MemoryAlignmentPolicy::Strict);
    auto dmac = map_sh4_dmac_registers(memory, scheduler, DmaTiming{1u});

    memory.write_u32(0x00u, 0x11223344u);
    memory.write_u32(0x04u, 0x55667788u);
    memory.write_u32(sh4_dmac_p4_base + 0x00u, 0x00u);
    memory.write_u32(sh4_dmac_p4_base + 0x04u, 0x40u);
    memory.write_u32(sh4_dmac_p4_base + 0x08u, 2u);
    memory.write_u32(sh4_dmac_p4_base + 0x0Cu,
                     auto_longword_increment | Sh4Dmac::interrupt_enable | Sh4Dmac::channel_enable);
    memory.write_u32(sh4_dmac_p4_base + 0x40u, Sh4Dmac::master_enable);

    const auto first = scheduler.advance_to(4u, 1u);
    require(first.processed_events == 1u && memory.read_u32(0x40u) == 0x11223344u &&
                memory.read_u32(0x44u) == 0u && dmac->count(0u) == 1u && dmac->source(0u) == 4u &&
                dmac->destination(0u) == 0x44u && !dmac->interrupt_pending(0u),
            "DMAC bildet Zwischenzustand oder gastzyklusgenaue Einzeltransfers nicht ab.");
    const auto second = scheduler.advance_to(8u, 1u);
    require(second.processed_events == 1u && memory.read_u32(0x44u) == 0x55667788u &&
                dmac->count(0u) == 0u && (dmac->control(0u) & Sh4Dmac::transfer_end) != 0u &&
                dmac->interrupt_pending(0u) && dmac->completed_transfer_units(0u) == 2u,
            "DMAC setzt DMATCR, TE oder IE nach Abschluss nicht korrekt.");
    memory.write_u32(sh4_dmac_area7_base + 0x0Cu,
                     auto_longword_increment | Sh4Dmac::interrupt_enable | Sh4Dmac::channel_enable);
    require(!dmac->interrupt_pending(0u) &&
                (memory.read_u32(sh4_dmac_area7_base + 0x0Cu) & Sh4Dmac::transfer_end) == 0u,
            "DMAC-TE laesst sich nicht ueber den Area-7-Alias quittieren.");

    dmac->reset();
    memory.write_u8(0x10u, 0xA0u);
    memory.write_u8(0x11u, 0xB1u);
    dmac->write_source(1u, 0x11u);
    dmac->write_destination(1u, 0x51u);
    dmac->write_count(1u, 1u);
    dmac->write_control(1u, 0x00000410u | Sh4Dmac::channel_enable);
    dmac->write_source(0u, 0x10u);
    dmac->write_destination(0u, 0x50u);
    dmac->write_count(0u, 1u);
    dmac->write_control(0u, 0x00000410u | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(9u, 1u));
    require(memory.read_u8(0x50u) == 0xA0u && memory.read_u8(0x51u) == 0u &&
                (dmac->control(0u) & Sh4Dmac::transfer_end) != 0u,
            "DMAC-Prioritaet bevorzugt bei gleichzeitigem Request nicht Kanal 0.");
    static_cast<void>(scheduler.advance_to(10u, 1u));
    require(memory.read_u8(0x51u) == 0xB1u,
            "DMAC verarbeitet nach dem Prioritaetsgewinner keinen Folgekanal.");

    dmac->reset();
    memory.write_u8(0x12u, 0xC2u);
    memory.write_u8(0x13u, 0xD3u);
    dmac->write_source(0u, 0x12u);
    dmac->write_destination(0u, 0x52u);
    dmac->write_count(0u, 2u);
    dmac->write_control(0u, 0x00000410u | Sh4Dmac::channel_enable);
    dmac->write_source(1u, 0x13u);
    dmac->write_destination(1u, 0x53u);
    dmac->write_count(1u, 1u);
    dmac->write_control(1u, 0x00000410u | Sh4Dmac::channel_enable);
    dmac->write_operation(0x00000300u | Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(12u, 2u));
    require(dmac->count(0u) == 1u && dmac->count(1u) == 0u && memory.read_u8(0x52u) == 0xC2u &&
                memory.read_u8(0x53u) == 0xD3u,
            "DMAC-Round-Robin gibt einem dauerhaft aktiven Kanal keinen fairen Folgeslot.");
    static_cast<void>(scheduler.advance_to(13u, 1u));
    require(dmac->count(0u) == 0u,
            "DMAC-Round-Robin kehrt nicht deterministisch zum verbleibenden Kanal zurueck.");

    dmac->reset();
    memory.write_u8(0x20u, 0xCCu);
    dmac->write_source(2u, 0x20u);
    dmac->write_destination(2u, 0x60u);
    dmac->write_count(2u, 1u);
    dmac->write_control(2u, 0x00000810u | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(20u, 0u));
    require(memory.read_u8(0x60u) == 0u, "Externer DMA-Request startet ohne Anforderung.");
    dmac->request_transfer(2u);
    static_cast<void>(scheduler.advance_to(21u, 1u));
    require(memory.read_u8(0x60u) == 0xCCu, "Expliziter DMA-Request startet den Kanal nicht.");

    dmac->reset();
    memory.write_u16(0xA0u, 0x1234u);
    dmac->write_source(0u, 0xA0u);
    dmac->write_destination(0u, 0xB0u);
    dmac->write_count(0u, 1u);
    dmac->write_control(0u, 0x00000420u | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(23u, 1u));
    require(memory.read_u16(0xB0u) == 0x1234u, "DMAC fuehrt 16-Bit-Transfers nicht aus.");

    dmac->reset();
    memory.write_u32(0xA8u, 0x01234567u);
    memory.write_u32(0xACu, 0x89ABCDEFu);
    dmac->write_source(0u, 0xA8u);
    dmac->write_destination(0u, 0xB8u);
    dmac->write_count(0u, 1u);
    dmac->write_control(0u, 0x00000400u | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(31u, 1u));
    require(memory.read_u32(0xB8u) == 0x01234567u && memory.read_u32(0xBCu) == 0x89ABCDEFu,
            "DMAC fuehrt 64-Bit-Transfers nicht vollstaendig aus.");

    dmac->reset();
    for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
        memory.write_u32(0x100u + offset, 0xA5000000u + offset);
    }
    dmac->write_source(0u, 0x100u);
    dmac->write_destination(0u, 0x120u);
    dmac->write_count(0u, 1u);
    dmac->write_control(0u, 0x00000440u | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(63u, 1u));
    bool block_matches = true;
    for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
        block_matches = block_matches && memory.read_u32(0x120u + offset) == 0xA5000000u + offset;
    }
    require(block_matches, "DMAC fuehrt 32-Byte-Blocktransfers nicht vollstaendig aus.");

    dmac->reset();
    memory.write_u32(0x184u, 0xCAFEBABEu);
    memory.write_u32(0x180u, 0x0BADF00Du);
    dmac->write_source(0u, 0x184u);
    dmac->write_destination(0u, 0x1C4u);
    dmac->write_count(0u, 2u);
    dmac->write_control(
        0u, 0x00008000u | 0x00002000u | 0x00000400u | 0x00000030u | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(71u, 2u));
    require(memory.read_u32(0x1C4u) == 0xCAFEBABEu && memory.read_u32(0x1C0u) == 0x0BADF00Du &&
                dmac->source(0u) == 0x17Cu && dmac->destination(0u) == 0x1BCu,
            "DMAC-Dekrementmodus oder Adressfeedback ist falsch.");

    dmac->reset();
    dmac->write_source(0u, 1u);
    dmac->write_destination(0u, 0x80u);
    dmac->write_count(0u, 1u);
    dmac->write_control(0u, auto_longword_increment | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    static_cast<void>(scheduler.advance_to(75u, 1u));
    require(dmac->address_error() && dmac->last_fault().has_value() &&
                dmac->last_fault()->reason == DmaFaultReason::MisalignedAddress &&
                (dmac->control(0u) & Sh4Dmac::transfer_end) == 0u && memory.read_u32(0x80u) == 0u,
            "DMAC-Adressfehler setzt AE nicht atomar und sichtbar.");
    dmac->write_operation(0u);
    require(!dmac->address_error() && !dmac->last_fault().has_value(),
            "DMAC-AE kann nicht mit Write-zero-to-clear quittiert werden.");

    dmac->reset();
    dmac->write_source(0u, 0u);
    dmac->write_destination(0u, 0x80u);
    dmac->write_count(0u, 1u);
    dmac->write_control(0u, auto_longword_increment | Sh4Dmac::channel_enable);
    dmac->write_operation(Sh4Dmac::master_enable);
    dmac->write_operation(0u);
    static_cast<void>(scheduler.advance_to(90u, 0u));
    require(memory.read_u32(0x80u) == 0u && dmac->completed_transfer_units(0u) == 0u,
            "Geloeschtes DMA-Master-Enable laesst ein geplantes Ereignis weiterlaufen.");

    {
        EventScheduler request_scheduler;
        Memory request_memory(256u, MemoryAlignmentPolicy::Strict);
        Sh4Dmac requests(request_scheduler, request_memory, DmaTiming{1u});
        request_memory.write_u8(0x10u, 0xA1u);
        requests.write_source(0u, 0x10u);
        requests.write_destination(0u, 0x40u);
        requests.write_count(0u, 1u);
        requests.write_control(0u, 0x00000810u | Sh4Dmac::channel_enable);
        requests.write_operation(Sh4Dmac::master_enable);
        requests.request_transfer(0u);
        requests.signal_nmi();
        requests.write_operation(Sh4Dmac::master_enable);
        static_cast<void>(request_scheduler.advance_to(4u, 0u));
        require(request_memory.read_u8(0x40u) == 0u,
                "Nach NMI wird ein alter externer DMA-Request ohne Neuanforderung fortgesetzt.");
        requests.request_transfer(0u);
        static_cast<void>(request_scheduler.advance_to(5u, 1u));
        require(request_memory.read_u8(0x40u) == 0xA1u,
                "Nach NMI startet ein neu ausgegebener externer Request nicht.");
    }

    {
        EventScheduler fault_scheduler;
        Memory fault_memory(256u, MemoryAlignmentPolicy::Strict);
        Sh4Dmac requests(fault_scheduler, fault_memory, DmaTiming{1u});
        fault_memory.write_u32(0x04u, 0xAABBCCDDu);
        requests.write_source(0u, 0x01u);
        requests.write_destination(0u, 0x40u);
        requests.write_count(0u, 1u);
        requests.write_control(0u, 0x00000830u | Sh4Dmac::channel_enable);
        requests.write_operation(Sh4Dmac::master_enable);
        requests.request_transfer(0u);
        requests.request_transfer(0u);
        static_cast<void>(fault_scheduler.advance_to(4u, 1u));
        require(requests.address_error(), "Externer DMA-Test erzeugt keinen Adressfehler.");
        requests.write_source(0u, 0x04u);
        requests.write_operation(Sh4Dmac::master_enable);
        static_cast<void>(fault_scheduler.advance_to(8u, 0u));
        require(fault_memory.read_u32(0x40u) == 0u,
                "Nach AE wird ein uebrig gebliebener externer DMA-Request fortgesetzt.");
        requests.request_transfer(0u);
        static_cast<void>(fault_scheduler.advance_to(12u, 1u));
        require(fault_memory.read_u32(0x40u) == 0xAABBCCDDu,
                "Nach AE startet ein neu ausgegebener externer Request nicht.");
    }

    {
        EventScheduler pause_scheduler;
        Memory pause_memory(256u, MemoryAlignmentPolicy::Strict);
        Sh4Dmac requests(pause_scheduler, pause_memory, DmaTiming{1u});
        pause_memory.write_u8(0x10u, 0x5Au);
        requests.write_source(0u, 0x10u);
        requests.write_destination(0u, 0x40u);
        requests.write_count(0u, 1u);
        requests.write_control(0u, 0x00000810u | Sh4Dmac::channel_enable);
        requests.write_operation(Sh4Dmac::master_enable);
        requests.request_transfer(0u);
        requests.write_operation(0u);
        static_cast<void>(pause_scheduler.advance_to(5u, 0u));
        require(pause_memory.read_u8(0x40u) == 0u,
                "DME=0 fuehrt einen pausierten externen Request trotzdem aus.");
        requests.write_operation(Sh4Dmac::master_enable);
        static_cast<void>(pause_scheduler.advance_to(6u, 1u));
        require(pause_memory.read_u8(0x40u) == 0x5Au,
                "DME=0 verwirft faelschlich den nur pausierten externen Request.");
    }

    {
        EventScheduler ddt_scheduler;
        Memory ddt_memory(256u, MemoryAlignmentPolicy::Strict);
        auto ddt = map_sh4_dmac_registers(ddt_memory, ddt_scheduler, DmaTiming{1u});
        ddt_memory.write_u8(0x10u, 0xD1u);
        ddt->write_source(1u, 0x10u);
        ddt->write_destination(1u, 0x40u);
        ddt->write_count(1u, 1u);
        ddt->write_control(1u, 0x00000810u | Sh4Dmac::channel_enable);

        require(!ddt->request_on_demand_transfer(1u) &&
                    !ddt->repeat_on_demand_transfer(),
                "DMAC nimmt DDT-Requests ausserhalb des DDT-Modus an.");
        ddt_memory.write_u32(sh4_dmac_area7_base + 0x40u,
                             Sh4Dmac::on_demand_enable | Sh4Dmac::master_enable);
        require(ddt_memory.read_u32(sh4_dmac_p4_base + 0x40u) ==
                    (Sh4Dmac::on_demand_enable | Sh4Dmac::master_enable),
                "DMAOR.DDT teilt Zustand oder Readback nicht ueber beide MMIO-Aliasse.");
        require(ddt->request_on_demand_transfer(1u) &&
                    ddt->pending_on_demand_requests(1u) == 1u,
                "DDT-Request wird im aktiven Modus nicht angenommen.");
        static_cast<void>(ddt_scheduler.advance_to(1u, 1u));
        require(ddt_memory.read_u8(0x40u) == 0xD1u &&
                    ddt->pending_on_demand_requests(1u) == 0u,
                "DDT-Request startet keinen schedulergebundenen Kanaltransfer.");

        ddt->write_control(1u, 0x00000810u | Sh4Dmac::channel_enable);
        require(ddt->repeat_on_demand_transfer(),
                "TR-only wiederholt den zuletzt angenommenen DDT-Kanal nicht.");
        ddt->write_operation(Sh4Dmac::master_enable);
        require(ddt->pending_on_demand_requests(1u) == 0u &&
                    !ddt->repeat_on_demand_transfer(),
                "Verlassen des DDT-Modus behaelt externe Queue oder Wiederholkanal.");

        ddt->write_operation(Sh4Dmac::on_demand_enable | Sh4Dmac::master_enable);
        for (std::uint32_t request = 0u; request < 4u; ++request) {
            require(ddt->request_on_demand_transfer(2u),
                    "DDT-Queue verwirft einen der vier dokumentierten Slots.");
        }
        require(!ddt->request_on_demand_transfer(2u) &&
                    ddt->pending_on_demand_requests(2u) == 4u,
                "DDT-Queue akzeptiert einen undokumentierten fuenften Request.");
        ddt->signal_nmi();
        require(ddt->pending_on_demand_requests(2u) == 0u,
                "NMI verwirft angenommene DDT-Requests nicht.");
    }

    {
        EventScheduler reference_scheduler;
        EventScheduler batch_scheduler;
        Memory reference_memory(1024u, MemoryAlignmentPolicy::Strict);
        Memory batch_memory(1024u, MemoryAlignmentPolicy::Strict);
        Sh4Dmac reference(reference_scheduler,
                          reference_memory,
                          DmaTiming{1u, 256u},
                          DmaExecutionMode::SingleUnitReference);
        Sh4Dmac batch(batch_scheduler,
                      batch_memory,
                      DmaTiming{1u, 256u},
                      DmaExecutionMode::DeterministicBatch);
        for (std::uint32_t index = 0u; index < 64u; ++index) {
            const auto value = 0xA5000000u | index;
            reference_memory.write_u32(index * 4u, value);
            batch_memory.write_u32(index * 4u, value);
        }
        for (auto* candidate : {&reference, &batch}) {
            candidate->write_source(0u, 0u);
            candidate->write_destination(0u, 512u);
            candidate->write_count(0u, 64u);
            candidate->write_control(0u, auto_longword_increment | Sh4Dmac::channel_enable);
            candidate->write_operation(Sh4Dmac::master_enable);
        }
        static_cast<void>(reference_scheduler.advance_to(256u, 64u));
        static_cast<void>(batch_scheduler.advance_to(256u, 1u));
        bool identical = true;
        for (std::uint32_t index = 0u; index < 64u; ++index) {
            identical = identical && reference_memory.read_u32(512u + index * 4u) ==
                                         batch_memory.read_u32(512u + index * 4u);
        }
        require(identical && reference.completed_transfer_units(0u) == 64u &&
                    batch.completed_transfer_units(0u) == 64u &&
                    batch.performance_counters().scheduler_callbacks <
                        reference.performance_counters().scheduler_callbacks,
                "Deterministischer DMA-Batch divergiert oder reduziert Schedulerarbeit nicht.");
    }

    require(throws<std::out_of_range>([&] { dmac->write_count(4u, 1u); }) &&
                throws<std::out_of_range>(
                    [&] { static_cast<void>(dmac->request_on_demand_transfer(4u)); }) &&
                throws<std::runtime_error>(
                    [&] { static_cast<void>(memory.read_u16(sh4_dmac_p4_base)); }),
            "DMAC akzeptiert ungueltige Kanaele oder schmale Registerzugriffe still.");

    std::cout << "KR-3103 DMA erfolgreich.\n";
    return 0;
}
