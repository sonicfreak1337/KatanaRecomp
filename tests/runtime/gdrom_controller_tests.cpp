#include "katana/runtime/gdrom_controller.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/holly_dma.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Operation> bool throws_runtime_error(Operation&& operation) {
    try {
        operation();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

class LayoutDiscSource final : public katana::runtime::DiscSource {
  public:
    LayoutDiscSource(std::vector<katana::runtime::DiscTrackLayout> layout, std::string identity)
        : layout_(std::move(layout)), identity_(std::move(identity)) {}
    [[nodiscard]] std::uint64_t size() const noexcept override { return 4096u; }
    [[nodiscard]] const std::string& identity() const noexcept override { return identity_; }
    [[nodiscard]] std::vector<katana::runtime::DiscTrackLayout> layout() const override {
        return layout_;
    }
    void read(const std::uint64_t offset, std::span<std::uint8_t> destination) const override {
        if (offset > size() || destination.size() > size() - offset)
            throw std::out_of_range("synthetic layout disc read");
        std::fill(destination.begin(), destination.end(), std::uint8_t{0xA5u});
    }

  private:
    std::vector<katana::runtime::DiscTrackLayout> layout_;
    std::string identity_;
};
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
    std::uint64_t command_acks = 0u;
    DreamcastGdRomController controller(
        cpu.memory,
        scheduler,
        GdRomDrive(source),
        [&](const std::uint64_t) { ++completions; },
        {},
        [&] { ++command_acks; });
    require(controller.reload_system_bootstrap(cpu) &&
                cpu.memory.read_u8(0x8C008100u) == 0x5Au &&
                cpu.memory.read_u8(0x8C00B8FFu) == 0x5Au,
            "System-Disc-Check laedt die sieben Bootstrapsektoren nicht neu.");

    controller.write(0x84u, 0x55u, MemoryAccessWidth::Byte);
    controller.write(0x88u, 0x66u, MemoryAccessWidth::Byte);
    controller.write(0x8Cu, 0x77u, MemoryAccessWidth::Byte);
    controller.write(0x90u, 0x34u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0x12u, MemoryAccessWidth::Byte);
    controller.write(0x98u, 0xB7u, MemoryAccessWidth::Byte);
    const auto taskfile_status = controller.status().ata_status;
    require(controller.read(0x18u, MemoryAccessWidth::Byte) == taskfile_status &&
                command_acks == 0u &&
                controller.read(0x8Cu, MemoryAccessWidth::Byte) == 0x77u &&
                controller.read(0x90u, MemoryAccessWidth::Byte) == 0x34u &&
                controller.read(0x94u, MemoryAccessWidth::Byte) == 0x12u &&
                controller.read(0x98u, MemoryAccessWidth::Byte) == 0xB0u,
            "Korrigierte GD-ROM-Taskfile-Offsets liefern nicht ihren sichtbaren Zustand.");
    require(controller.read(0x9Cu, MemoryAccessWidth::Byte) == taskfile_status &&
                command_acks == 1u,
            "Nur der normale Command-Status-Lesezugriff quittiert das GD-ROM-Ereignis nicht.");
    require(throws_runtime_error(
                [&] { static_cast<void>(controller.read(0xA0u, MemoryAccessWidth::Byte)); }) &&
                throws_runtime_error(
                    [&] { controller.write(0xA0u, 0u, MemoryAccessWidth::Byte); }),
            "Der veraltete GD-ROM-Device-Control-Offset 0xA0 bleibt faelschlich aktiv.");
    controller.reset();

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

    controller.reset();
    constexpr std::uint32_t parameters = 0x8C003000u;
    constexpr std::uint32_t destination = 0x8C004000u;
    constexpr std::uint32_t extended_status = 0x8C005000u;
    cpu.memory.write_u32(parameters, 150u);
    cpu.memory.write_u32(parameters + 4u, 1u);
    cpu.memory.write_u32(parameters + 8u, destination);
    cpu.r[4] = 16u;
    cpu.r[5] = parameters;
    const auto read_request = controller.bios_call(cpu, 0u, 0u);
    cpu.r[4] = read_request;
    cpu.r[5] = extended_status;
    require(read_request >= 1u && controller.bios_call(cpu, 1u, 0u) == 1u,
            "REQ_CMD oder GET_CMD_STAT meldet keinen eingereihten Request.");
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    require(controller.bios_call(cpu, 1u, 0u) == 1u,
            "EXEC_SERVER schliesst einen asynchronen Read sofort ab.");
    static_cast<void>(scheduler.advance_by(1'500u, 1u));
    require(controller.bios_call(cpu, 1u, 0u) == 2u &&
                cpu.memory.read_u32(extended_status) == 0u &&
                cpu.memory.read_u32(extended_status + 4u) == 0u &&
                cpu.memory.read_u32(extended_status + 8u) == 2048u &&
                cpu.memory.read_u32(extended_status + 12u) == 0u &&
                cpu.memory.read_u8(destination) == 0x5Au,
            "GET_CMD_STAT liefert keinen einmaligen Vierwortstatus mit Bytezahl.");
    require(controller.bios_call(cpu, 1u, 0u) == 0u,
            "Abgeholter BIOS-Request bleibt faelschlich aktiv.");

    cpu.r[4] = 0x777u;
    cpu.r[5] = 0u;
    const auto invalid_request = controller.bios_call(cpu, 0u, 0u);
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    cpu.r[4] = invalid_request;
    cpu.r[5] = extended_status;
    require(invalid_request >= 1u && controller.bios_call(cpu, 1u, 0u) == 0xFFFFFFFFu &&
                cpu.memory.read_u32(extended_status) == 5u &&
                controller.last_bios_request().id == invalid_request &&
                controller.last_bios_request().command == 0x777u &&
                controller.last_bios_request().state == GdRomBiosRequestState::Error &&
                controller.last_bios_request().status[0] == 5u,
            "Unbekanntes BIOS-Kommando wird nicht kontrolliert als Illegal Request abgelehnt.");
    require(!controller.bios_call_events().empty() &&
                controller.bios_call_events().back().request_id == invalid_request &&
                controller.bios_call_events().back().state_before ==
                    GdRomBiosRequestState::Error &&
                controller.bios_call_events().back().state_after ==
                    GdRomBiosRequestState::Error &&
                controller.bios_call_events().back().result == 0xFFFFFFFFu &&
                controller.format_bios_call_events_json().find("\"status\":[5,") !=
                    std::string::npos,
            "Sequenziertes GD-ROM-BIOS-Ereignislog verliert Requestzustand oder Vierwortstatus.");

    constexpr std::uint32_t aborted_destination = 0x8C020000u;
    cpu.memory.write_u32(parameters, 150u);
    cpu.memory.write_u32(parameters + 4u, 1u);
    cpu.memory.write_u32(parameters + 8u, aborted_destination);
    cpu.r[4] = 16u;
    cpu.r[5] = parameters;
    const auto aborted_request = controller.bios_call(cpu, 0u, 0u);
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    constexpr std::uint32_t busy_drive_status = 0x8C00A180u;
    cpu.r[4] = busy_drive_status;
    require(controller.bios_call(cpu, 4u, 0u) == 0u &&
                cpu.memory.read_u32(busy_drive_status) == 0u &&
                cpu.memory.read_u32(busy_drive_status + 4u) == 0u,
            "GD-ROM-Drive-Status meldet einen laufenden Read nicht als BUSY.");
    cpu.r[4] = aborted_request;
    cpu.r[5] = 0u;
    const auto completions_before_abort = completions;
    require(controller.bios_call(cpu, 8u, 0u) == 0u,
            "GD-ROM READ_ABORT lehnt einen aktiven Request ab.");
    cpu.memory.write_u32(extended_status, 0xCAFEBABEu);
    cpu.r[5] = extended_status;
    require(controller.bios_call(cpu, 1u, 0u) == 0u &&
                cpu.memory.read_u32(extended_status) == 0xCAFEBABEu &&
                controller.last_bios_request().state == GdRomBiosRequestState::Aborted &&
                controller.bios_call(cpu, 8u, 0u) == 0xFFFFFFFFu,
            "Abgebrochener GD-ROM-Request verschwindet nicht als NOT_FOUND.");
    static_cast<void>(scheduler.advance_by(2'000u, 1u));
    require(completions == completions_before_abort &&
                cpu.memory.read_u8(aborted_destination) == 0u,
            "Abgebrochener GD-ROM-Read schreibt spaeter Daten oder meldet Completion.");

    cpu.r[4] = 0x8C010200u;
    cpu.r[5] = 0x12345678u;
    require(controller.bios_call(cpu, 5u, 0u) == 0xFFFFFFFFu,
            "GD-ROM-DMA-IRQ-Handoff wird ohne abgeschlossene DMA akzeptiert.");
    cpu.r[4] = 0x8C010240u;
    cpu.r[5] = 0x87654321u;
    require(controller.bios_call(cpu, 11u, 0u) == 0u &&
                controller.status().dma_callback == 0u &&
                controller.status().dma_callback_argument == 0u &&
                controller.status().pio_callback == 0x8C010240u &&
                controller.status().pio_callback_argument == 0x87654321u &&
                controller.bios_call_events().back().request_id == 0u,
            "GD-ROM-Callbackvertraege trennen DMA-Handoff und PIO-Registrierung nicht.");

    constexpr std::uint32_t sector_mode = 0x8C00A000u;
    cpu.memory.write_u32(sector_mode, 0u);
    cpu.memory.write_u32(sector_mode + 4u, 0x2000u);
    cpu.memory.write_u32(sector_mode + 8u, 1024u);
    cpu.memory.write_u32(sector_mode + 12u, 2048u);
    cpu.r[4] = sector_mode;
    require(controller.bios_call(cpu, 10u, 0u) == 0u,
            "Gueltiger GD-ROM-Datentyp wird abgelehnt.");
    cpu.memory.write_u32(sector_mode, 1u);
    cpu.memory.write_u32(sector_mode + 4u, 0u);
    cpu.memory.write_u32(sector_mode + 8u, 0u);
    cpu.memory.write_u32(sector_mode + 12u, 0u);
    require(controller.bios_call(cpu, 10u, 0u) == 0u &&
                cpu.memory.read_u32(sector_mode) == 1u &&
                cpu.memory.read_u32(sector_mode + 4u) == 0x2000u &&
                cpu.memory.read_u32(sector_mode + 8u) == 1024u &&
                cpu.memory.read_u32(sector_mode + 12u) == 2048u,
            "GD-ROM-Datentyp-Query verliert den gesetzten Vierwortvertrag.");
    cpu.memory.write_u32(sector_mode, 0u);
    cpu.memory.write_u32(sector_mode + 4u, 0x1000u);
    cpu.memory.write_u32(sector_mode + 8u, 0u);
    cpu.memory.write_u32(sector_mode + 12u, 2352u);
    require(controller.bios_call(cpu, 10u, 0u) == 0xFFFFFFFFu &&
                controller.status().sector_mode[1] == 0x2000u &&
                controller.status().sector_mode[2] == 1024u &&
                controller.status().sector_mode[3] == 2048u,
            "Ungueltiger GD-ROM-Datentyp mutiert den aktiven Modus.");

    cpu.r[4] = 0u;
    require(controller.bios_call(cpu, 9u, 0u) == 0u &&
                controller.status().dma_callback == 0u &&
                controller.status().pio_callback == 0x8C010240u &&
                controller.status().sector_mode[1] == 0x2000u &&
                controller.status().sector_mode[3] == 2048u,
            "GD-ROM RESET loescht faelschlich den BIOS-Callback- oder Datentypvertrag.");

    constexpr std::uint32_t drive_status = 0x8C00A100u;
    cpu.r[4] = drive_status;
    require(controller.bios_call(cpu, 4u, 0u) == 0u &&
                cpu.memory.read_u32(drive_status) == 1u &&
                cpu.memory.read_u32(drive_status + 4u) == 0x80u,
            "GD-ROM-Drive-Status meldet fuer eine eingelegte GD-ROM keinen Pausezustand.");

    constexpr std::uint32_t reset_destination = 0x8C021000u;
    cpu.memory.write_u32(parameters, 150u);
    cpu.memory.write_u32(parameters + 4u, 1u);
    cpu.memory.write_u32(parameters + 8u, reset_destination);
    cpu.r[4] = 16u;
    cpu.r[5] = parameters;
    const auto reset_pending_request = controller.bios_call(cpu, 0u, 0u);
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    const auto completions_before_init = completions;
    require(reset_pending_request >= 1u && controller.bios_call(cpu, 3u, 0u) == 0u &&
                controller.status().bios_requests == 0u &&
                controller.status().dma_callback == 0u &&
                controller.status().pio_callback == 0u &&
                controller.status().sector_mode[1] == 0x2000u &&
                controller.status().sector_mode[2] == 1024u &&
                controller.status().sector_mode[3] == 2048u,
            "INIT_SYSTEM setzt Queue, Callback- oder Datentypzustand nicht zurueck.");
    static_cast<void>(scheduler.advance_by(2'000u, 1u));
    require(completions == completions_before_init &&
                cpu.memory.read_u8(reset_destination) == 0u,
            "INIT_SYSTEM laesst einen alten GD-ROM-Request spaeter abschliessen.");

    CpuState stream_cpu;
    stream_cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(stream_cpu.memory));
    EventScheduler stream_scheduler;
    std::vector<std::uint8_t> stream_bytes(4u * 2048u);
    for (std::size_t index = 0u; index < stream_bytes.size(); ++index)
        stream_bytes[index] = static_cast<std::uint8_t>((index * 73u + 0x31u) & 0xFFu);
    auto stream_source =
        std::make_shared<MemoryDiscSource>(stream_bytes, "synthetic-streaming-gdrom");
    std::uint64_t stream_drive_completions = 0u;
    DreamcastGdRomController stream_controller(
        stream_cpu.memory,
        stream_scheduler,
        GdRomDrive(std::move(stream_source)),
        [&](const std::uint64_t) { ++stream_drive_completions; });
    std::vector<SystemAsicEvent> stream_g1_events;
    DreamcastG1BusController stream_g1(
        stream_scheduler,
        HollyDmaTiming{4u},
        [&](const std::uint32_t address,
            const std::uint32_t length,
            const std::uint32_t direction) {
            stream_controller.dma_to_memory(address, length, direction);
        },
        [&](const SystemAsicEvent event) { stream_g1_events.push_back(event); });
    stream_controller.bind_g1_bus(&stream_g1);

    constexpr std::uint32_t stream_parameters = 0x8C030000u;
    constexpr std::uint32_t stream_status = 0x8C030020u;
    constexpr std::uint32_t stream_transfer = 0x8C030040u;
    constexpr std::uint32_t stream_residue = 0x8C030060u;
    const auto queue_ready_stream = [&](const std::uint32_t command,
                                        const std::uint32_t fad,
                                        const std::uint32_t sector_count = 1u) {
        stream_cpu.memory.write_u32(stream_parameters, fad);
        stream_cpu.memory.write_u32(stream_parameters + 4u, sector_count);
        stream_cpu.memory.write_u32(stream_parameters + 8u, 0u);
        stream_cpu.memory.write_u32(stream_parameters + 12u, 0u);
        stream_cpu.r[4] = command;
        stream_cpu.r[5] = stream_parameters;
        const auto request = stream_controller.bios_call(stream_cpu, 0u, 0u);
        stream_cpu.r[4] = request;
        stream_cpu.r[5] = stream_status;
        require(request >= 1u && stream_controller.bios_call(stream_cpu, 1u, 0u) == 1u,
                "Streaming-REQ_CMD wird nicht als PROCESSING eingereiht.");
        static_cast<void>(stream_controller.bios_call(stream_cpu, 2u, 0u));
        stream_cpu.r[4] = request;
        stream_cpu.r[5] = stream_status;
        const auto completions_before_ready = stream_drive_completions;
        static_cast<void>(stream_scheduler.advance_by(999u, 1u));
        require(stream_drive_completions == completions_before_ready &&
                    stream_controller.bios_call(stream_cpu, 1u, 0u) == 1u,
                "Streaming-Request wird vor seiner Gastzeit bereit.");
        static_cast<void>(stream_scheduler.advance_by(1u, 1u));
        stream_cpu.r[4] = request;
        stream_cpu.r[5] = stream_status;
        require(stream_drive_completions == completions_before_ready + 1u &&
                    stream_controller.bios_call(stream_cpu, 1u, 0u) == 3u &&
                    stream_cpu.memory.read_u32(stream_status + 8u) == 0u &&
                    stream_controller.status().stream_bytes_remaining ==
                        static_cast<std::uint64_t>(sector_count) * 2048u,
                "Streaming-Request erreicht nach der Gastzeit keinen STREAMING-Zustand.");
        return request;
    };
    const auto stream_memory_matches = [&](const std::uint32_t destination,
                                           const std::size_t source_offset,
                                           const std::size_t length) {
        for (std::size_t index = 0u; index < length; ++index) {
            if (stream_cpu.memory.read_u8(destination + static_cast<std::uint32_t>(index)) !=
                stream_bytes[source_offset + index])
                return false;
        }
        return true;
    };

    constexpr std::uint32_t dma_stream_destination = 0x8C040000u;
    constexpr std::uint32_t dma_callback_address = 0x8C010280u;
    constexpr std::uint32_t dma_callback_argument = 0x2468ACE0u;
    const std::vector<std::uint8_t> untouched_stream(4096u, 0xCCu);
    stream_cpu.memory.write_bytes(dma_stream_destination, untouched_stream);
    const auto dma_stream_request = queue_ready_stream(28u, 150u, 2u);
    stream_cpu.r[4] = dma_callback_address;
    stream_cpu.r[5] = dma_callback_argument;
    require(stream_controller.bios_call(stream_cpu, 5u, 0u) == 0xFFFFFFFFu &&
                stream_controller.status().dma_callback == 0u &&
                !stream_controller.take_pending_guest_callback(),
            "DMA-IRQ-Callbackselector akzeptiert faelschlich eine Vorabregistrierung.");
    stream_cpu.memory.write_u32(stream_transfer, dma_stream_destination);
    stream_cpu.memory.write_u32(stream_transfer + 4u, 4096u);
    stream_cpu.r[4] = dma_stream_request;
    stream_cpu.r[5] = stream_transfer;
    require(stream_controller.bios_call(stream_cpu, 6u, 0u) == 0u &&
                stream_g1.state().active == 1u && stream_g1.state().enabled == 1u &&
                stream_g1.state().direction == 1u && stream_g1.state().remaining == 4096u &&
                stream_controller.status().transfer_bytes_remaining == 4096u,
            "BIOS-DMA-Streaming startet keinen G1-Transfer in Laufwerk-zu-System-Richtung.");
    stream_cpu.r[4] = dma_stream_request;
    stream_cpu.r[5] = stream_residue;
    require(stream_controller.bios_call(stream_cpu, 7u, 0u) == 1u &&
                stream_cpu.memory.read_u32(stream_residue) == 0u,
            "BIOS-DMA-Streaming meldet vor dem ersten Chunk keinen Null-Fortschritt.");
    static_cast<void>(stream_scheduler.advance_by(8191u, 1u));
    require(stream_cpu.memory.read_u8(dma_stream_destination) == 0xCCu &&
                stream_g1_events.empty() &&
                stream_controller.status().transfer_bytes_remaining == 4096u,
            "BIOS-DMA-Streaming schreibt vor dem ersten gastzeitgebundenen G1-Chunk.");
    static_cast<void>(stream_scheduler.advance_by(1u, 1u));
    stream_cpu.r[4] = dma_stream_request;
    stream_cpu.r[5] = stream_residue;
    require(stream_memory_matches(dma_stream_destination, 0u, 2048u) &&
                stream_cpu.memory.read_u8(dma_stream_destination + 2048u) == 0xCCu &&
                stream_g1.state().system_counter == dma_stream_destination + 2048u &&
                stream_g1.state().peripheral_counter == 2048u &&
                stream_controller.bios_call(stream_cpu, 7u, 0u) == 1u &&
                stream_cpu.memory.read_u32(stream_residue) == 2048u &&
                stream_controller.status().stream_bytes_remaining == 2048u,
            "Erster BIOS-DMA-Streamingchunk aktualisiert Daten oder Residue nicht exakt.");
    static_cast<void>(stream_scheduler.advance_by(8192u, 1u));
    require(stream_memory_matches(dma_stream_destination, 0u, 4096u) &&
                stream_g1.state().active == 0u && stream_g1.state().remaining == 0u &&
                stream_g1.state().system_counter == dma_stream_destination + 4096u &&
                stream_g1.state().peripheral_counter == 4096u &&
                stream_g1_events ==
                    std::vector<SystemAsicEvent>{SystemAsicEvent::GdromDma} &&
                stream_controller.status().completed_dma == 1u &&
                stream_controller.status().stream_bytes_remaining == 0u &&
                stream_controller.status().transfer_bytes_remaining == 0u,
            "BIOS-DMA-Streaming liefert nicht exakt einen finalen IRQ und exakte Disc-Daten.");
    stream_cpu.r[4] = dma_callback_address;
    stream_cpu.r[5] = dma_callback_argument;
    require(stream_controller.bios_call(stream_cpu, 5u, 0u) == 0u &&
                stream_controller.status().pending_guest_callbacks == 1u,
            "DMA-IRQ-Callbackselector nimmt die abgeschlossene DMA nicht entgegen.");
    const auto dma_callback = stream_controller.take_pending_guest_callback();
    require(dma_callback && dma_callback->kind == GdRomBiosTransferKind::Dma &&
                dma_callback->address == dma_callback_address &&
                dma_callback->argument == dma_callback_argument &&
                dma_callback->request_id == dma_stream_request &&
                !stream_controller.take_pending_guest_callback() &&
                stream_controller.bios_call(stream_cpu, 5u, 0u) == 0xFFFFFFFFu,
            "DMA-IRQ-Callback wird nicht genau einmal nach Completion ausgeliefert.");
    stream_cpu.r[4] = dma_stream_request;
    stream_cpu.r[5] = stream_residue;
    require(stream_controller.bios_call(stream_cpu, 7u, 0u) == 0u &&
                stream_cpu.memory.read_u32(stream_residue) == 0u,
            "Abgeschlossenes BIOS-DMA-Streaming behaelt eine falsche Transferresidue.");
    stream_cpu.r[4] = dma_stream_request;
    stream_cpu.r[5] = stream_status;
    require(stream_controller.bios_call(stream_cpu, 1u, 0u) == 2u &&
                stream_cpu.memory.read_u32(stream_status) == 0u &&
                stream_cpu.memory.read_u32(stream_status + 4u) == 0u &&
                stream_cpu.memory.read_u32(stream_status + 8u) == 4096u &&
                stream_cpu.memory.read_u32(stream_status + 12u) == 0u &&
                stream_controller.bios_call(stream_cpu, 1u, 0u) == 0u,
            "Finaler DMA-Streamingstatus ist nicht einmalig COMPLETED mit exakter Bytezahl.");

    constexpr std::uint32_t pio_callback_address = 0x8C010240u;
    constexpr std::uint32_t pio_callback_argument = 0x13579BDFu;
    stream_cpu.r[4] = pio_callback_address;
    stream_cpu.r[5] = pio_callback_argument;
    require(stream_controller.bios_call(stream_cpu, 11u, 0u) == 0u,
            "PIO-Streamingcallback kann nicht registriert werden.");
    constexpr std::uint32_t pio_stream_destination = 0x8C041000u;
    stream_cpu.memory.write_bytes(pio_stream_destination, untouched_stream);
    const auto pio_stream_request = queue_ready_stream(37u, 151u, 2u);
    stream_cpu.memory.write_u32(stream_transfer, pio_stream_destination);
    stream_cpu.memory.write_u32(stream_transfer + 4u, 2048u);
    stream_cpu.r[4] = pio_stream_request;
    stream_cpu.r[5] = stream_transfer;
    require(stream_controller.bios_call(stream_cpu, 12u, 0u) == 0u &&
                stream_memory_matches(pio_stream_destination, 2048u, 2048u) &&
                stream_cpu.memory.read_u8(pio_stream_destination + 2048u) == 0xCCu &&
                stream_controller.status().stream_bytes_remaining == 2048u &&
                stream_controller.status().transfer_bytes_remaining == 0u &&
                stream_controller.status().pending_guest_callbacks == 1u,
            "BIOS-PIO-Streaming schreibt nicht synchron exakte Daten oder verliert Callback.");
    stream_cpu.r[4] = pio_stream_request;
    stream_cpu.r[5] = stream_residue;
    require(stream_controller.bios_call(stream_cpu, 13u, 0u) == 0u &&
                stream_cpu.memory.read_u32(stream_residue) == 2048u,
            "Abgeschlossener PIO-Chunk meldet nicht den verbleibenden Gesamtstream.");
    const auto pio_callback = stream_controller.take_pending_guest_callback();
    require(pio_callback && pio_callback->kind == GdRomBiosTransferKind::Pio &&
                pio_callback->address == pio_callback_address &&
                pio_callback->argument == pio_callback_argument &&
                pio_callback->request_id == pio_stream_request &&
                !stream_controller.take_pending_guest_callback(),
            "PIO-Streamingcallback wird nicht genau einmal mit Request und Argument geliefert.");

    stream_cpu.memory.write_u32(stream_transfer, pio_stream_destination + 2048u);
    stream_cpu.memory.write_u32(stream_transfer + 4u, 2048u);
    stream_cpu.r[4] = pio_stream_request;
    stream_cpu.r[5] = stream_transfer;
    require(stream_controller.bios_call(stream_cpu, 12u, 0u) == 0u &&
                stream_memory_matches(pio_stream_destination + 2048u, 4096u, 2048u) &&
                stream_controller.status().stream_bytes_remaining == 0u &&
                stream_controller.status().pending_guest_callbacks == 1u,
            "Zweiter PIO-Streamingchunk beendet den Stream nicht mit exakten Daten.");
    stream_cpu.r[4] = pio_stream_request;
    stream_cpu.r[5] = stream_residue;
    require(stream_controller.bios_call(stream_cpu, 13u, 0u) == 0u &&
                stream_cpu.memory.read_u32(stream_residue) == 0u,
            "Finaler PIO-Streamingchunk behaelt eine falsche Gesamtresidue.");
    const auto final_pio_callback = stream_controller.take_pending_guest_callback();
    require(final_pio_callback &&
                final_pio_callback->kind == GdRomBiosTransferKind::Pio &&
                final_pio_callback->address == pio_callback_address &&
                final_pio_callback->argument == pio_callback_argument &&
                final_pio_callback->request_id == pio_stream_request &&
                !stream_controller.take_pending_guest_callback(),
            "Persistenter PIO-Callback wird nicht fuer jeden Transferrequest geliefert.");
    stream_cpu.r[4] = pio_stream_request;
    stream_cpu.r[5] = stream_status;
    require(stream_controller.bios_call(stream_cpu, 1u, 0u) == 2u &&
                stream_cpu.memory.read_u32(stream_status + 8u) == 4096u,
            "Finaler PIO-Streamingstatus enthaelt nicht die exakte Bytezahl.");

    constexpr std::uint32_t aborted_stream_destination = 0x8C042000u;
    stream_cpu.memory.write_bytes(aborted_stream_destination, untouched_stream);
    const auto aborted_stream_request = queue_ready_stream(28u, 152u, 2u);
    stream_cpu.memory.write_u32(stream_transfer, aborted_stream_destination);
    stream_cpu.memory.write_u32(stream_transfer + 4u, 4096u);
    stream_cpu.r[4] = aborted_stream_request;
    stream_cpu.r[5] = stream_transfer;
    require(stream_controller.bios_call(stream_cpu, 6u, 0u) == 0u,
            "Abort-Regression kann den DMA-Streamingtransfer nicht starten.");
    stream_cpu.r[4] = dma_callback_address;
    stream_cpu.r[5] = dma_callback_argument;
    require(stream_controller.bios_call(stream_cpu, 5u, 0u) == 0xFFFFFFFFu &&
                !stream_controller.take_pending_guest_callback(),
            "Abort-Regression akzeptiert einen DMA-Callback vor dem echten IRQ.");
    static_cast<void>(stream_scheduler.advance_by(8192u, 1u));
    require(stream_memory_matches(aborted_stream_destination, 4096u, 2048u) &&
                stream_cpu.memory.read_u8(aborted_stream_destination + 2048u) == 0xCCu,
            "Abort-Regression erreicht keinen einzelnen bewiesenen DMA-Chunk.");
    const auto g1_events_before_abort = stream_g1_events.size();
    stream_cpu.r[4] = aborted_stream_request;
    stream_cpu.r[5] = 0u;
    require(stream_controller.bios_call(stream_cpu, 8u, 0u) == 0u &&
                stream_g1.state().active == 0u && !stream_g1.state().completion_event &&
                stream_g1.state().remaining == 0u,
            "READ_ABORT stoppt den laufenden G1-Streamingtransfer nicht sofort.");
    static_cast<void>(stream_scheduler.advance_by(16'384u, 256u));
    stream_cpu.r[4] = dma_callback_address;
    stream_cpu.r[5] = dma_callback_argument;
    require(stream_controller.bios_call(stream_cpu, 5u, 0u) == 0xFFFFFFFFu &&
                !stream_controller.take_pending_guest_callback(),
            "Abgebrochene DMA erzeugt nachtraeglich einen quittierbaren Callback.");
    stream_cpu.r[4] = aborted_stream_request;
    stream_cpu.r[5] = stream_status;
    require(stream_g1_events.size() == g1_events_before_abort &&
                stream_memory_matches(aborted_stream_destination, 4096u, 2048u) &&
                stream_cpu.memory.read_u8(aborted_stream_destination + 2048u) == 0xCCu &&
                stream_controller.bios_call(stream_cpu, 1u, 0u) == 0u &&
                !stream_controller.take_pending_guest_callback() &&
                stream_controller.status().pending_guest_callbacks == 0u,
            "READ_ABORT laesst spaete DMA-Chunks, IRQs oder Gastcallbacks durch.");

    CpuState toc_cpu;
    toc_cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(toc_cpu.memory));
    EventScheduler toc_scheduler;
    auto toc_source = std::make_shared<LayoutDiscSource>(
        std::vector<DiscTrackLayout>{{1u, 0u, DiscTrackKind::Audio, 2352u, 100u, 1u},
                                     {2u, 100u, DiscTrackKind::Audio, 2352u, 100u, 1u},
                                     {3u, 45'000u, DiscTrackKind::Data, 2048u, 100u, 2u}},
        "synthetic-two-area-gdrom");
    DreamcastGdRomController toc_controller(
        toc_cpu.memory, toc_scheduler, GdRomDrive(std::move(toc_source)));
    const auto request_toc = [&](const std::uint32_t area, const std::uint32_t output) {
        toc_cpu.memory.write_u32(parameters, area);
        toc_cpu.memory.write_u32(parameters + 4u, output);
        toc_cpu.r[4] = 19u;
        toc_cpu.r[5] = parameters;
        const auto request = toc_controller.bios_call(toc_cpu, 0u, 0u);
        toc_cpu.r[4] = request;
        toc_cpu.r[5] = extended_status;
        require(toc_controller.bios_call(toc_cpu, 1u, 0u) == 1u,
                "BIOS-TOC ist vor EXEC_SERVER nicht PROCESSING.");
        static_cast<void>(toc_controller.bios_call(toc_cpu, 2u, 0u));
        require(toc_controller.bios_call(toc_cpu, 1u, 0u) == 2u &&
                    toc_cpu.memory.read_u32(extended_status + 8u) == 408u,
                "BIOS-TOC meldet nicht exakt 102 Gastwoerter.");
    };
    constexpr std::uint32_t low_toc = 0x8C006000u;
    constexpr std::uint32_t high_toc = 0x8C007000u;
    request_toc(0u, low_toc);
    request_toc(1u, high_toc);
    require(toc_cpu.memory.read_u32(low_toc) != 0xFFFFFFFFu &&
                toc_cpu.memory.read_u32(low_toc + 4u) != 0xFFFFFFFFu &&
                toc_cpu.memory.read_u32(low_toc + 8u) == 0xFFFFFFFFu &&
                toc_cpu.memory.read_u32(high_toc) == 0xFFFFFFFFu &&
                toc_cpu.memory.read_u32(high_toc + 4u) == 0xFFFFFFFFu &&
                toc_cpu.memory.read_u32(high_toc + 8u) != 0xFFFFFFFFu &&
                toc_cpu.memory.read_u32(low_toc + 99u * 4u) !=
                    toc_cpu.memory.read_u32(high_toc + 99u * 4u),
            "LOW- und HIGH-BIOS-TOC trennen ihre Trackbereiche nicht.");
    return EXIT_SUCCESS;
}
