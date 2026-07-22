#include "katana/runtime/gdrom_controller.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
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
    require(controller.bios_call(cpu, 5u, 0u) == 0u,
            "GD-ROM-DMA-Callback kann nicht registriert werden.");
    cpu.r[4] = 0x8C010240u;
    cpu.r[5] = 0x87654321u;
    require(controller.bios_call(cpu, 11u, 0u) == 0u &&
                controller.status().dma_callback == 0x8C010200u &&
                controller.status().dma_callback_argument == 0x12345678u &&
                controller.status().pio_callback == 0x8C010240u &&
                controller.status().pio_callback_argument == 0x87654321u &&
                controller.bios_call_events().back().request_id == 0u,
            "GD-ROM-Callbackregistrierung ist kein stabiler Geraetezustand.");

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
                controller.status().dma_callback == 0x8C010200u &&
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
