#include "katana/runtime/gdrom_controller.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    using namespace katana::runtime;
    CpuState cpu;
    cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(cpu.memory));
    EventScheduler scheduler;
    std::vector<std::uint8_t> bytes(8u * 2048u, 0x5Au);
    auto source = std::make_shared<MemoryDiscSource>(bytes, "synthetic-gdrom");
    std::uint64_t completions = 0u;
    DreamcastGdRomController controller(
        cpu.memory, scheduler, GdRomDrive(source), [&](const std::uint64_t) { ++completions; });
    require(controller.reload_system_bootstrap(cpu) &&
                cpu.memory.read_u8(0x8C008100u) == 0x5Au &&
                cpu.memory.read_u8(0x8C00B8FFu) == 0x5Au,
            "System-Disc-Check laedt die sieben Bootstrapsektoren nicht neu.");

    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    std::array<std::uint8_t, 12u> inquiry{};
    inquiry[0] = 0x12u;
    for (std::size_t offset = 0u; offset < inquiry.size(); offset += 2u) {
        controller.write(0x80u,
                         static_cast<std::uint16_t>(inquiry[offset]) |
                             (static_cast<std::uint16_t>(inquiry[offset + 1u]) << 8u),
                         MemoryAccessWidth::Halfword);
    }
    require(controller.status().ata_status == 0x80u && completions == 0u &&
                controller.status().completed_commands == 0u,
            "GD-ROM-Paketkommando schliesst synchron beim letzten Paketwort ab.");
    static_cast<void>(scheduler.advance_by(999u, 1u));
    require(completions == 0u, "GD-ROM-Paketcompletion wird vor dem Zielzyklus signalisiert.");
    static_cast<void>(scheduler.advance_by(1u, 1u));
    require(completions == 1u && controller.status().completed_commands == 1u &&
                controller.status().pio_bytes_available == 36u,
            "GD-ROM-Paketcompletion publiziert Daten oder Observer nicht asynchron.");

    controller.reset();
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    std::array<std::uint8_t, 12u> invalid{};
    invalid[0] = 0xFFu;
    for (std::size_t offset = 0u; offset < invalid.size(); offset += 2u) {
        controller.write(0x80u,
                         static_cast<std::uint16_t>(invalid[offset]) |
                             (static_cast<std::uint16_t>(invalid[offset + 1u]) << 8u),
                         MemoryAccessWidth::Halfword);
    }
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require(completions == 2u && (controller.status().ata_status & 1u) != 0u &&
                controller.status().interrupt_reason == 3u,
            "Normaler GD-ROM-Kommandofehler wird nicht als ATA-Fehler abgeschlossen.");
    return EXIT_SUCCESS;
}
