#include "katana/runtime/aica.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;
    AicaExecutionController execution;
    require(execution.mode() == AicaArm7Mode::HighLevelAudio &&
                !execution.arm7_executes_instructions(),
            "AICA startet nicht im dokumentierten HLE-Audioprofil.");
    require(throws<std::runtime_error>([&] { execution.set_mode(AicaArm7Mode::LowLevelArm7); }) &&
                execution.mode() == AicaArm7Mode::HighLevelAudio,
            "Nicht implementiertes ARM7-LLE wird still akzeptiert oder veraendert den Modus.");

    execution.timer(0u).configure(254u, 1u, true);
    execution.interrupts().set_enabled(AicaExecutionController::timer_interrupt_base);
    execution.tick(3u);
    require(execution.timer(0u).counter() == 255u && !execution.interrupts().asserted(),
            "AICA-Timer ignoriert Teiler oder loest zu frueh aus.");
    execution.tick(1u);
    require(execution.timer(0u).counter() == 0u && execution.interrupts().asserted() &&
                execution.interrupts().pending() == (1u << 6u),
            "AICA-Timeroverflow erzeugt keinen maskierten Interrupt.");
    execution.interrupts().acknowledge(1u << 6u);
    require(!execution.interrupts().asserted() && execution.interrupts().pending() == 0u,
            "AICA-Timerinterrupt kann nicht quittiert werden.");
    std::uint32_t dma_requests = 0u;
    execution.set_dma_request_observer([&] { ++dma_requests; });
    execution.tick(256u);
    require(dma_requests == 0u,
            "Ein normaler AICA-Audiotick wird faelschlich als G2-DMA-Request ausgegeben.");
    execution.request_dma();
    require(dma_requests == 1u,
            "Ein expliziter AICA-DMA-Request erreicht den G2-Hardwaretrigger nicht.");
    execution.timer(1u).configure(255u, 0u, true);
    execution.tick(1u);
    require(execution.interrupts().pending() == (1u << 7u) && !execution.interrupts().asserted(),
            "Maskierter AICA-Interrupt geht verloren oder wird faelschlich zugestellt.");
    require(throws<std::out_of_range>([&] { static_cast<void>(execution.timer(3u)); }),
            "Ungueltiger AICA-Timerindex wird akzeptiert.");
    require(throws<std::invalid_argument>([] {
                AicaTimer timer;
                timer.configure(0u, 8u, true);
            }),
            "Ungueltiger AICA-Timerteiler wird akzeptiert.");

    Memory memory(0u);
    auto sound_ram = map_dreamcast_aica_ram(memory);
    auto product_execution = std::make_shared<AicaExecutionController>();
    auto product_registers = map_aica_registers(memory, product_execution, sound_ram);
    memory.write_u32(0x00702C00u, 0x0000ABCDu);
    require(product_execution->arm7_reset_asserted() &&
                memory.read_u32(0x00702C00u) == 0x0000AB01u,
            "AICA-ARM-Resetbit und VREG sind nicht mit dem ExecutionController verbunden.");
    memory.write_u8(0x00702C00u, 0u);
    require(!product_execution->arm7_reset_asserted() && memory.read_u8(0x00702C00u) == 0u &&
                !product_execution->arm7_executes_instructions(),
            "AICA-ARM-Freigabe wird nicht verfolgt oder behauptet faelschlich ARM7-LLE.");
    sound_ram->write_u16(0u, 1000u);
    sound_ram->write_u16(2u, std::bit_cast<std::uint16_t>(std::int16_t{-1000}));
    memory.write_u32(0x00702800u, 0x0Fu);
    memory.write_u32(0x00700004u, 0u);
    memory.write_u32(0x0070000Cu, 2u);
    memory.write_u32(0x00700018u, 0u);
    memory.write_u8(0x00700024u, 0u);
    memory.write_u8(0x00700025u, 0x0Fu);
    memory.write_u8(0x00700029u, 0u);
    memory.write_u32(0x00700000u, 0x0000C000u);
    const auto rendered = product_registers->render_audio(2u, 44'100u);
    require(rendered == std::vector<std::int16_t>({1000, 1000, -1000, -1000}) &&
                product_registers->active_channel_count() == 0u &&
                product_registers->rendered_buffer_count() == 1u,
            "AICA-Gastregister, gemeinsames Sound-RAM und Produktmixer sind nicht verbunden.");
    memory.write_u32(0x00700000u, 0x00008000u);
    require(product_registers->render_audio(1u, 44'100u) ==
                std::vector<std::int16_t>({0, 0}),
            "AICA-Key-Off stoppt den ausgefuehrten Produktkanal nicht.");

    std::cout << "KR-2904 ARM7-HLE-Strategie, Timer und Interrupts erfolgreich.\n";
}
