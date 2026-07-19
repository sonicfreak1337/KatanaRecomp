#include "katana/runtime/maple_mmio.hpp"

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
    auto ram = std::make_shared<LinearMemoryDevice>(16u * 1024u * 1024u);
    memory.map_region("test-main-ram", 0x0C000000u, ram);
    EventScheduler scheduler;
    auto maple = std::make_shared<MapleBus>();
    ControllerState state;
    state.pressed_buttons = static_cast<std::uint16_t>(ControllerButton::A);
    auto input = std::make_shared<ReplayInputBackend>(std::vector<ControllerState>{state});
    maple->attach(0u, 0u, std::make_shared<MapleControllerDevice>(input));
    std::uint64_t completions = 0u;
    const auto controller = map_dreamcast_maple_controller(
        memory, scheduler, maple, MapleDmaTiming{10u}, [&] { ++completions; });

    constexpr std::uint32_t table = 0x0C001000u;
    constexpr std::uint32_t response = 0x0C002000u;
    constexpr std::uint32_t request_header = 0x01002009u;
    memory.write_u32(table, 0x80000001u);
    memory.write_u32(table + 4u, response);
    memory.write_u32(table + 8u, request_header);
    memory.write_u32(table + 12u, 0x01000000u);

    memory.write_u32(0xA05F6C04u, table);
    memory.write_u32(0x805F6C10u, 0u);
    memory.write_u32(0x005F6C14u, 1u);
    memory.write_u32(0xA05F6C18u, 1u);
    require(memory.read_u32(0x005F6C18u) == 1u && scheduler.pending_event_count() == 1u,
            "Maple-DMA-Start plant keinen sichtbaren asynchronen Transfer.");
    static_cast<void>(scheduler.advance_by(100u, 1u));
    require(memory.read_u32(0x005F6C18u) == 0u && completions == 1u &&
                controller->completed_dma_count() == 1u &&
                controller->transferred_word_count() == 6u,
            "Maple-DMA schliesst nicht einmalig mit Zaehlern und Status ab.");
    require(memory.read_u32(response) == 0x03200008u &&
                memory.read_u32(response + 4u) == 0x01000000u &&
                (memory.read_u32(response + 8u) &
                 static_cast<std::uint16_t>(ControllerButton::A)) == 0u,
            "Maple-DMA schreibt keinen korrekt adressierten Controller-Responseframe.");

    memory.write_u32(0x005F6C80u, 0xFFFFFFFFu);
    memory.write_u32(0x005F6CE8u, 0u);
    require(memory.read_u32(0xA05F6C80u) == 0xFFFF130Fu && memory.read_u32(0x805F6CE8u) == 0u &&
                memory.read_u32(0x005F6C84u) == 0u,
            "Maple-System-, MSB- oder Statusregister besitzen falsche Semantik.");
    memory.write_u32(0x005F6C8Cu, 0x12340000u);
    memory.write_u32(0x005F6C8Cu, 0x61557F00u);
    require(throws([&] { static_cast<void>(memory.read_u32(0x005F6C8Cu)); }) &&
                throws([&] { memory.write_u32(0x005F6C84u, 1u); }) &&
                throws([&] { static_cast<void>(memory.read_u32(0x005F6C00u)); }) &&
                throws([&] { memory.write_u32(0x005F6C00u, 1u); }) &&
                throws([&] { static_cast<void>(memory.read_u16(0x005F6C04u)); }),
            "Maple-MMIO-Zugriffsrechte oder Breitenvertrag sind offen.");

    memory.write_u32(table, 0x80000100u);
    memory.write_u32(0x005F6C04u, table);
    memory.write_u32(0x005F6C14u, 1u);
    require(throws([&] { memory.write_u32(0x005F6C18u, 1u); }),
            "Unbekanntes Maple-DMA-Deskriptormuster wurde akzeptiert.");

    memory.write_u32(table, 0x80000000u);
    memory.write_u32(table + 4u, 0x0C002000u);
    memory.write_u32(table + 8u, 0x01002009u);
    require(throws([&] { memory.write_u32(0x005F6C18u, 1u); }),
            "Widerspruechliche Maple-Frame-/Deskriptorlaenge wurde akzeptiert.");

    std::cout << "Dreamcast-Maple-MMIO und echter DMA-Responsepfad erfolgreich.\n";
}
