#include "katana/runtime/scif.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function> bool fails(Function&& function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace katana::runtime;
    EventScheduler scheduler;
    Memory memory(0u);
    std::array<bool, 4u> interrupts{};
    const auto scif = map_sh4_scif(
        memory,
        scheduler,
        [&](const Sh4ScifInterrupt source, const bool pending) {
            interrupts[static_cast<std::size_t>(source)] = pending;
        });

    require(memory.read_u16(sh4_scif_p4_base + 0x10u) == 0x0060u &&
                memory.read_u8(sh4_scif_area7_base + 0x04u) == 0xFFu,
            "SCIF-Resetwerte oder gemeinsamer Area-7-Alias fehlen.");
    memory.write_u16(sh4_scif_p4_base + 0x20u, 0xFFFFu);
    require(memory.read_u16(sh4_scif_area7_base + 0x20u) == 0x00E3u,
            "SCSPTR2 maskiert reservierte Bits oder den aktiven CTS-Eingang falsch.");

    memory.write_u16(sh4_scif_p4_base + 0x00u, 0u);
    memory.write_u8(sh4_scif_p4_base + 0x04u, 0u);
    memory.write_u16(sh4_scif_p4_base + 0x08u, 0x00A0u);
    memory.write_u8(sh4_scif_p4_base + 0x0Cu, 0x5Au);
    require(scif->transmit_fifo_size() == 1u && interrupts[3u] &&
                scheduler.next_event_cycle() == 1280u,
            "SCIF-Sendefifo, TXI oder gastzeitliche Framefrist sind nicht verbunden.");
    const auto transmitted = scheduler.advance_to(1280u, 1u);
    require(transmitted.processed_events == 1u && scif->transmitted_bytes().size() == 1u &&
                scif->transmitted_bytes().front() == 0x5Au && scif->transmit_fifo_size() == 0u,
            "SCIF uebertraegt FIFO-Daten nicht deterministisch in Gastzeit.");

    memory.write_u16(sh4_scif_p4_base + 0x08u, 0x0050u);
    scif->inject_receive(0xA5u);
    require(scif->receive_fifo_size() == 1u && interrupts[1u] &&
                memory.read_u16(sh4_scif_p4_base + 0x1Cu) == 1u &&
                memory.read_u8(sh4_scif_p4_base + 0x14u) == 0xA5u,
            "SCIF-Empfangsfifo, Zaehler oder RXI sind nicht verbunden.");
    require(scif->receive_fifo_size() == 0u && !interrupts[1u],
            "Leeres SCIF-Empfangsfifo laesst RXI haengen.");

    require(fails([&] { memory.write_u32(sh4_scif_p4_base + 0x20u, 0u); }) &&
                fails([&] { memory.write_u8(sh4_scif_p4_base + 0x14u, 0u); }) &&
                fails([&] { static_cast<void>(memory.read_u8(sh4_scif_p4_base + 0x0Cu)); }),
            "SCIF akzeptiert falsche Breiten oder Read-/Write-only-Zugriffe.");

    std::cout << "SH-4-SCIF-Register, FIFO, Gastzeit und Aliase erfolgreich.\n";
}
