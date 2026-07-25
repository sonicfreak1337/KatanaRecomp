#include "katana/runtime/aica.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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
    const auto register_snapshot = product_registers->snapshot();
    require(rendered == std::vector<std::int16_t>({1000, 1000, -1000, -1000}) &&
                product_registers->active_channel_count() == 0u &&
                product_registers->rendered_buffer_count() == 1u &&
                register_snapshot.registers[aica_common_register_base] == 0x0Fu &&
                register_snapshot.registers[0u] == 0u &&
                register_snapshot.registers[1u] == 0x40u &&
                register_snapshot.channels[0u].phase == (std::uint64_t{2u} << 32u) &&
                !register_snapshot.channels[0u].active &&
                register_snapshot.rendered_buffers == 1u &&
                register_snapshot.rendered_frames == 2u,
            "AICA-Gastregister, gemeinsames Sound-RAM und Produktmixer sind nicht verbunden.");
    memory.write_u32(0x00700000u, 0x00008000u);
    require(product_registers->render_audio(1u, 44'100u) ==
                std::vector<std::int16_t>({0, 0}),
            "AICA-Key-Off stoppt den ausgefuehrten Produktkanal nicht.");

    auto fault_ram = std::make_shared<LinearMemoryDevice>(8u);
    AicaRegisterFile fault_registers({}, fault_ram);
    const auto configure_voice = [&](const std::size_t channel,
                                     const std::uint16_t control,
                                     const std::uint16_t sample_base,
                                     const std::uint16_t loop_end) {
        const auto base = static_cast<std::uint32_t>(
            channel * aica_channel_register_stride);
        fault_registers.write(base + 4u, sample_base, MemoryAccessWidth::Halfword);
        fault_registers.write(base + 8u, 0u, MemoryAccessWidth::Halfword);
        fault_registers.write(base + 12u, loop_end, MemoryAccessWidth::Halfword);
        fault_registers.write(base + 24u, 0u, MemoryAccessWidth::Halfword);
        fault_registers.write(base + 36u, 0x0F00u, MemoryAccessWidth::Halfword);
        fault_registers.write(base + 41u, 0u, MemoryAccessWidth::Byte);
        fault_registers.write(base, control, MemoryAccessWidth::Halfword);
    };
    fault_ram->write_u16(0u, 100u);
    fault_ram->write_u16(2u, 200u);
    fault_ram->write_u16(4u, 300u);
    fault_ram->write_u16(6u, 0u);
    fault_registers.write(
        aica_common_register_base, 0x0Fu, MemoryAccessWidth::Byte);
    configure_voice(0u, 0x4000u, 6u, 2u);
    configure_voice(1u, 0xC000u, 0u, 2u);
    const auto pcm_fault_audio = fault_registers.render_audio(2u, 44'100u);
    const auto pcm_fault_snapshot = fault_registers.snapshot();
    require(pcm_fault_audio == std::vector<std::int16_t>({100, 100, 200, 200}) &&
                pcm_fault_snapshot.voice_errors == 1u &&
                pcm_fault_snapshot.first_voice_error &&
                pcm_fault_snapshot.first_voice_error->error ==
                    AicaVoiceError::Pcm16OutOfRange &&
                pcm_fault_snapshot.first_voice_error->channel == 0u &&
                pcm_fault_snapshot.first_voice_error->sample_address == 8u &&
                pcm_fault_snapshot.first_voice_error->rendered_frame == 1u &&
                !pcm_fault_snapshot.channels[0u].active &&
                pcm_fault_snapshot.channels[1u].phase ==
                    (std::uint64_t{2u} << 32u),
            "Ungueltiger PCM-Bereich beendet den Audiopfad oder die gueltige Nachbarvoice.");

    fault_registers.reset();
    fault_registers.write(
        aica_common_register_base, 0x0Fu, MemoryAccessWidth::Byte);
    configure_voice(0u, 0x4100u, 7u, 3u);
    configure_voice(1u, 0xC000u, 0u, 3u);
    static_cast<void>(fault_registers.render_audio(3u, 44'100u));
    const auto adpcm_fault_snapshot = fault_registers.snapshot();
    require(adpcm_fault_snapshot.voice_errors == 1u &&
                adpcm_fault_snapshot.first_voice_error &&
                adpcm_fault_snapshot.first_voice_error->error ==
                    AicaVoiceError::AdpcmOutOfRange &&
                adpcm_fault_snapshot.first_voice_error->format ==
                    AicaSampleFormat::Adpcm4 &&
                adpcm_fault_snapshot.first_voice_error->channel == 0u &&
                adpcm_fault_snapshot.first_voice_error->sample_address == 8u &&
                adpcm_fault_snapshot.first_voice_error->rendered_frame == 2u &&
                adpcm_fault_snapshot.channels[1u].phase ==
                    (std::uint64_t{3u} << 32u),
            "Ungueltiger ADPCM-Bereich deaktiviert nicht nur die betroffene Voice.");

    auto byte_execution = std::make_shared<AicaExecutionController>();
    auto half_execution = std::make_shared<AicaExecutionController>();
    auto word_execution = std::make_shared<AicaExecutionController>();
    AicaRegisterFile byte_registers(byte_execution);
    AicaRegisterFile half_registers(half_execution);
    AicaRegisterFile word_registers(word_execution);
    const auto exercise_partial_writes =
        [](AicaRegisterFile& registers,
           AicaExecutionController& controller,
           const MemoryAccessWidth width) {
            const auto write_logical_word =
                [&](const std::uint32_t offset, const std::uint32_t value) {
                    if (width == MemoryAccessWidth::Word) {
                        registers.write(offset, value, width);
                        return;
                    }
                    const auto chunk = static_cast<std::size_t>(width);
                    for (std::size_t byte = 0u; byte < sizeof(value); byte += chunk)
                        registers.write(
                            offset + static_cast<std::uint32_t>(byte),
                            value >> (byte * 8u),
                            width);
                };
            controller.interrupts().request(
                AicaExecutionController::timer_interrupt_base);
            write_logical_word(0u, 0x0000C000u);
            write_logical_word(0x2890u, 0x000002FEu);
            write_logical_word(
                0x28B4u, AicaExecutionController::timer_interrupt_base);
            write_logical_word(
                0x28BCu, AicaExecutionController::timer_interrupt_base);
            write_logical_word(0x2C00u, 1u);
        };
    exercise_partial_writes(
        byte_registers, *byte_execution, MemoryAccessWidth::Byte);
    exercise_partial_writes(
        half_registers, *half_execution, MemoryAccessWidth::Halfword);
    exercise_partial_writes(
        word_registers, *word_execution, MemoryAccessWidth::Word);
    const auto byte_snapshot = byte_registers.snapshot();
    const auto half_snapshot = half_registers.snapshot();
    const auto word_snapshot = word_registers.snapshot();
    require(byte_snapshot.registers == half_snapshot.registers &&
                byte_snapshot.registers == word_snapshot.registers &&
                byte_snapshot.channels == half_snapshot.channels &&
                byte_snapshot.channels == word_snapshot.channels &&
                byte_execution->snapshot() == half_execution->snapshot() &&
                byte_execution->snapshot() == word_execution->snapshot(),
            "AICA-Byte-, Halfword- und Wordwrites mit identischen Endbytes divergieren.");
    require(byte_snapshot.channels[0u].active &&
                byte_execution->timer(0u).counter() == 0xFEu &&
                byte_execution->timer(0u).snapshot().divisor == 4u &&
                byte_execution->interrupts().enabled() ==
                    AicaExecutionController::timer_interrupt_base &&
                byte_execution->interrupts().pending() == 0u &&
                byte_execution->arm7_reset_asserted(),
            "Timerhighbyte oder Channel-Controlhighbyte aktualisiert die AICA-Semantik nicht.");

    EventScheduler reset_scheduler;
    auto reset_execution =
        std::make_shared<AicaExecutionController>(&reset_scheduler);
    require(reset_scheduler.pending_event_count() == 1u &&
                reset_execution->snapshot().tick_event,
            "AICA-Execution plant beim Start nicht genau einen Audiotick.");
    reset_execution->timer(0u).configure(253u, 2u, true);
    reset_execution->timer(1u).configure(17u, 0u, true);
    reset_execution->interrupts().set_enabled(0xFFFFFFFFu);
    reset_execution->interrupts().request(
        AicaExecutionController::timer_interrupt_base);
    reset_execution->set_arm7_reset_asserted(true);
    reset_execution->reset();
    const auto direct_reset = reset_execution->snapshot();
    require(direct_reset.mode == AicaArm7Mode::HighLevelAudio &&
                !direct_reset.arm7_reset_asserted &&
                direct_reset.timers ==
                    std::array<AicaTimer::Snapshot,
                               AicaExecutionController::timer_count>{} &&
                direct_reset.interrupts == AicaInterruptState::Snapshot{} &&
                direct_reset.tick_event &&
                direct_reset.error == AicaExecutionError::None &&
                reset_scheduler.pending_event_count() == 1u,
            "AICA-Executionreset raeumt Timer, Interrupts, ARM7 oder Tickplanung nicht.");

    reset_execution->timer(2u).configure(99u, 3u, true);
    reset_execution->interrupts().set_enabled(0xFFFFFFFFu);
    reset_execution->interrupts().request(
        AicaExecutionController::timer_interrupt_base << 2u);
    reset_execution->set_arm7_reset_asserted(true);
    reset_scheduler.reset();
    const auto scheduler_reset = reset_execution->snapshot();
    require(scheduler_reset.timers ==
                    std::array<AicaTimer::Snapshot,
                               AicaExecutionController::timer_count>{} &&
                scheduler_reset.interrupts == AicaInterruptState::Snapshot{} &&
                !scheduler_reset.arm7_reset_asserted &&
                scheduler_reset.tick_event &&
                scheduler_reset.error == AicaExecutionError::None &&
                reset_scheduler.pending_event_count() == 1u,
            "AICA-Schedulerreset behaelt Ausfuehrungszustand oder dupliziert den Audiotick.");

    AicaRegisterFile reset_registers(reset_execution);
    reset_execution->timer(0u).configure(123u, 1u, true);
    reset_execution->interrupts().set_enabled(0xFFFFFFFFu);
    reset_execution->interrupts().request(
        AicaExecutionController::timer_interrupt_base);
    reset_execution->set_arm7_reset_asserted(true);
    reset_registers.reset();
    require(reset_execution->timer(0u).snapshot() == AicaTimer::Snapshot{} &&
                reset_execution->interrupts().snapshot() ==
                    AicaInterruptState::Snapshot{} &&
                !reset_execution->arm7_reset_asserted() &&
                reset_scheduler.pending_event_count() == 1u,
            "AICA-Registerreset verwendet nicht den vollstaendigen Executionreset.");

    const auto live_tick = reset_execution->snapshot().tick_event;
    require(live_tick && reset_scheduler.cancel(*live_tick),
            "AICA-Schedulerfehlertest kann den aktuellen Tick nicht isolieren.");
    static_cast<void>(reset_scheduler.advance_to(
        std::numeric_limits<std::uint64_t>::max(), 0u));
    reset_execution->reset();
    const auto failed_tick_reset = reset_execution->snapshot();
    require(!failed_tick_reset.tick_event &&
                failed_tick_reset.error ==
                    AicaExecutionError::TickScheduleFailure &&
                reset_scheduler.pending_event_count() == 0u,
            "AICA-Reset verschluckt einen Tick-Schedulerfehler ohne sichtbaren Zustand.");
    reset_scheduler.reset();
    const auto recovered_tick_reset = reset_execution->snapshot();
    require(recovered_tick_reset.tick_event &&
                recovered_tick_reset.error == AicaExecutionError::None &&
                reset_scheduler.pending_event_count() == 1u,
            "AICA erholt sich nach Schedulerreset nicht vom sichtbaren Tickfehler.");

    std::cout << "KR-2904 ARM7-HLE-Strategie, Timer und Interrupts erfolgreich.\n";
}
