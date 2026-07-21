#include "katana/runtime/holly_dma.hpp"

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
    require(!g1_bytes_committed && !g1_completed && g1.read(0x18u) == 1u,
            "G1-DMA macht Daten bereits beim Start sichtbar.");
    static_cast<void>(g1_scheduler.advance_by(127u, 1u));
    require(!g1_bytes_committed, "G1-DMA committed Daten vor dem faelligen Schedulerzyklus.");
    static_cast<void>(g1_scheduler.advance_by(1u, 1u));
    require(g1_bytes_committed && g1_completed && g1.read(0x18u) == 0u,
            "G1-DMA committed Daten oder ASIC-Completion nicht atomar am Zielzyklus.");

    memory.write_u32(0x005F7418u, 0u);
    memory.write_u32(0x005F7414u, 1u);
    require(throws([&] { memory.write_u32(0x005F7418u, 1u); }) &&
                throws([&] { static_cast<void>(memory.read_u32(0x005F7400u)); }) &&
                throws([&] { static_cast<void>(memory.read_u16(0x005F7800u)); }) &&
                throws([&] { memory.write_u32(0x005F7880u, 1u); }) &&
                throws([&] { memory.write_u32(0x005F78A0u, 1u); }),
            "G1-/G2-DMA-Gates, reservierte Register oder Breitenvertrag sind offen.");

    std::cout << "Dreamcast-G1/G2-DMA-Register und echte DMA-Pfade erfolgreich.\n";
}
