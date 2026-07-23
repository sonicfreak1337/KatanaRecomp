#include "katana/runtime/gdrom_controller.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/holly_dma.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

template <typename Operation> bool throws_out_of_range(Operation&& operation) {
    try {
        operation();
    } catch (const std::out_of_range&) {
        return true;
    }
    return false;
}

katana::runtime::BlockExit dormant_aot_block(katana::runtime::CpuState&,
                                             katana::runtime::BlockExecutionContext&) {
    katana::runtime::BlockExit exit;
    exit.kind = katana::runtime::BlockEndKind::Return;
    return exit;
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
    const auto cpu_storage = std::make_unique<CpuState>();
    auto& cpu = *cpu_storage;
    cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(cpu.memory));
    std::uint64_t rejected_mmio_writes = 0u;
    auto rejected_mmio = std::make_shared<MmioMemoryDevice>(
        0x1000u,
        [](const std::uint32_t, const MemoryAccessWidth) { return 0u; },
        [&](const std::uint32_t, const std::uint32_t, const MemoryAccessWidth) {
            ++rejected_mmio_writes;
        });
    cpu.memory.map_region("gdrom-rejected-bios-mmio", 0x005F0000u, rejected_mmio);
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
        [&] { ++command_acks; },
        {},
        {},
        DiscLoadExecutionPolicy::StandaloneTestMode);
    const auto write_packet = [](DreamcastGdRomController& target,
                                 const std::array<std::uint8_t, 12u>& packet) {
        for (std::size_t offset = 0u; offset < packet.size(); offset += 2u) {
            target.write(0x80u,
                         static_cast<std::uint16_t>(packet[offset]) |
                             (static_cast<std::uint16_t>(packet[offset + 1u]) << 8u),
                         MemoryAccessWidth::Halfword);
        }
    };
    const auto append_pio_word = [](DreamcastGdRomController& target,
                                    std::vector<std::uint8_t>& output,
                                    const std::size_t valid_bytes = 2u) {
        const auto word = target.read(0x80u, MemoryAccessWidth::Halfword);
        output.push_back(static_cast<std::uint8_t>(word));
        if (valid_bytes == 2u) output.push_back(static_cast<std::uint8_t>(word >> 8u));
    };
    const auto issue_cd_read = [&](DreamcastGdRomController& target,
                                   EventScheduler& target_scheduler,
                                   const std::uint8_t features,
                                   const std::uint32_t fad,
                                   const std::uint32_t sector_count,
                                   const std::uint16_t pio_byte_limit = 0u) {
        target.write(0x84u, features, MemoryAccessWidth::Byte);
        target.write(0x90u, pio_byte_limit & 0xFFu, MemoryAccessWidth::Byte);
        target.write(0x94u, pio_byte_limit >> 8u, MemoryAccessWidth::Byte);
        target.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
        std::array<std::uint8_t, 12u> packet{};
        packet[0] = 0x30u;
        packet[2] = static_cast<std::uint8_t>(fad >> 16u);
        packet[3] = static_cast<std::uint8_t>(fad >> 8u);
        packet[4] = static_cast<std::uint8_t>(fad);
        packet[8] = static_cast<std::uint8_t>(sector_count >> 16u);
        packet[9] = static_cast<std::uint8_t>(sector_count >> 8u);
        packet[10] = static_cast<std::uint8_t>(sector_count);
        write_packet(target, packet);
        static_cast<void>(target_scheduler.advance_by(1'000u, 1u));
    };
    const auto read_taskfile_sense = [&](DreamcastGdRomController& target,
                                         EventScheduler& target_scheduler) {
        static_cast<void>(target.read(0x9Cu, MemoryAccessWidth::Byte));
        target.write(0x90u, 10u, MemoryAccessWidth::Byte);
        target.write(0x94u, 0u, MemoryAccessWidth::Byte);
        target.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
        std::array<std::uint8_t, 12u> packet{};
        packet[0] = 0x13u;
        packet[4] = 10u;
        write_packet(target, packet);
        static_cast<void>(target_scheduler.advance_by(1'000u, 1u));
        static_cast<void>(target.read(0x9Cu, MemoryAccessWidth::Byte));
        std::vector<std::uint8_t> sense;
        for (std::size_t index = 0u; index < 5u; ++index)
            append_pio_word(target, sense);
        static_cast<void>(target.read(0x9Cu, MemoryAccessWidth::Byte));
        return sense;
    };
    {
        CpuState timeout_cpu;
        timeout_cpu.memory = Memory(0u);
        static_cast<void>(map_dreamcast_main_ram(timeout_cpu.memory));
        EventScheduler timeout_scheduler;
        static_cast<void>(timeout_scheduler.advance_to(
            std::numeric_limits<std::uint64_t>::max() - 999u, 0u));
        std::uint64_t timeout_irqs = 0u;
        DreamcastGdRomController timeout_controller(
            timeout_cpu.memory,
            timeout_scheduler,
            GdRomDrive(
                std::make_shared<MemoryDiscSource>(bytes, "synthetic-taskfile-timeout")),
            [&](const std::uint64_t) { ++timeout_irqs; },
            {},
            {},
            {},
            {},
            DiscLoadExecutionPolicy::StandaloneTestMode);

        timeout_controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
        std::array<std::uint8_t, 12u> timeout_packet{};
        timeout_packet[0] = 0x00u;
        write_packet(timeout_controller, timeout_packet);
        require(timeout_controller.status().ata_status == 0x41u &&
                    timeout_controller.status().interrupt_reason == 3u &&
                    timeout_controller.status().completed_commands == 1u &&
                    timeout_controller.read(0x84u, MemoryAccessWidth::Byte) == 0xB4u &&
                    timeout_irqs == 1u,
                "GD-ROM-Paket-Schedulerueberlauf entkommt als Hostexception oder bleibt BSY.");
        static_cast<void>(timeout_controller.read(0x9Cu, MemoryAccessWidth::Byte));

        timeout_cpu.r[4] = 29u;
        timeout_cpu.r[5] = 0u;
        const auto recovered_request = timeout_controller.bios_call(timeout_cpu, 0u, 0u);
        static_cast<void>(timeout_controller.bios_call(timeout_cpu, 2u, 0u));
        timeout_cpu.r[4] = recovered_request;
        timeout_cpu.r[5] = 0u;
        require(recovered_request >= 1u &&
                    timeout_controller.bios_call(timeout_cpu, 1u, 0u) == 2u,
                "Taskfile-Schedulerfehler gibt den Laufwerksbesitz nicht fuer Folgerequests frei.");
    }
    {
        CpuState timeout_cpu;
        timeout_cpu.memory = Memory(0u);
        static_cast<void>(map_dreamcast_main_ram(timeout_cpu.memory));
        EventScheduler timeout_scheduler;
        static_cast<void>(timeout_scheduler.advance_to(
            std::numeric_limits<std::uint64_t>::max() - 999u, 0u));
        std::uint64_t timeout_irqs = 0u;
        DreamcastGdRomController timeout_controller(
            timeout_cpu.memory,
            timeout_scheduler,
            GdRomDrive(std::make_shared<MemoryDiscSource>(bytes, "synthetic-bios-timeout")),
            [&](const std::uint64_t) { ++timeout_irqs; },
            {},
            {},
            {},
            {},
            DiscLoadExecutionPolicy::StandaloneTestMode);
        constexpr std::uint32_t timeout_parameters = 0x8C001000u;
        constexpr std::uint32_t timeout_status = 0x8C001020u;
        constexpr std::uint32_t timeout_destination = 0x8C002000u;

        timeout_cpu.memory.write_u32(timeout_parameters, 150u);
        timeout_cpu.memory.write_u32(timeout_parameters + 4u, 1u);
        timeout_cpu.memory.write_u32(timeout_parameters + 8u, timeout_destination);
        timeout_cpu.memory.write_u32(timeout_parameters + 12u, 0u);
        timeout_cpu.memory.write_u32(timeout_destination, 0xA55AA55Au);
        timeout_cpu.r[4] = 16u;
        timeout_cpu.r[5] = timeout_parameters;
        const auto read_request = timeout_controller.bios_call(timeout_cpu, 0u, 0u);
        static_cast<void>(timeout_controller.bios_call(timeout_cpu, 2u, 0u));
        timeout_cpu.r[4] = read_request;
        timeout_cpu.r[5] = timeout_status;
        require(read_request >= 1u &&
                    timeout_controller.bios_call(timeout_cpu, 1u, 0u) == 0xFFFFFFFFu &&
                    timeout_cpu.memory.read_u32(timeout_status) == 0x0Bu &&
                    timeout_cpu.memory.read_u32(timeout_status + 4u) ==
                        static_cast<std::uint32_t>(GdRomStatus::Aborted) &&
                    timeout_cpu.memory.read_u32(timeout_status + 8u) == 0u &&
                    timeout_cpu.memory.read_u32(timeout_status + 12u) == 0u &&
                    timeout_cpu.memory.read_u32(timeout_destination) == 0xA55AA55Au &&
                    timeout_irqs == 0u,
                "BIOS-Read-Schedulerueberlauf entkommt oder schreibt vor der Fehlergrenze.");

        timeout_cpu.memory.write_u32(timeout_parameters, 150u);
        timeout_cpu.memory.write_u32(timeout_parameters + 4u, 1u);
        timeout_cpu.memory.write_u32(timeout_parameters + 8u, 0u);
        timeout_cpu.memory.write_u32(timeout_parameters + 12u, 0u);
        timeout_cpu.r[4] = 28u;
        timeout_cpu.r[5] = timeout_parameters;
        const auto stream_request = timeout_controller.bios_call(timeout_cpu, 0u, 0u);
        static_cast<void>(timeout_controller.bios_call(timeout_cpu, 2u, 0u));
        timeout_cpu.r[4] = stream_request;
        timeout_cpu.r[5] = timeout_status;
        require(stream_request > read_request &&
                    timeout_controller.bios_call(timeout_cpu, 1u, 0u) == 0xFFFFFFFFu &&
                    timeout_cpu.memory.read_u32(timeout_status) == 0x0Bu &&
                    timeout_cpu.memory.read_u32(timeout_status + 4u) ==
                        static_cast<std::uint32_t>(GdRomStatus::Aborted) &&
                    timeout_cpu.memory.read_u32(timeout_status + 8u) == 0u &&
                    timeout_cpu.memory.read_u32(timeout_status + 12u) == 0u &&
                    timeout_irqs == 0u,
                "BIOS-Stream-Schedulerueberlauf entkommt oder verliert seinen Fehlerstatus.");

        timeout_cpu.r[4] = 29u;
        timeout_cpu.r[5] = 0u;
        const auto recovered_request = timeout_controller.bios_call(timeout_cpu, 0u, 0u);
        static_cast<void>(timeout_controller.bios_call(timeout_cpu, 2u, 0u));
        timeout_cpu.r[4] = recovered_request;
        timeout_cpu.r[5] = 0u;
        require(recovered_request > stream_request &&
                    timeout_controller.bios_call(timeout_cpu, 1u, 0u) == 2u &&
                    timeout_controller.status().bios_requests == 0u,
                "BIOS-Schedulerfehler blockiert einen unabhaengigen Folgerequest.");
    }
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
                command_acks == 0u,
            "Ein normaler Command-Status-Lesezugriff erfindet ohne IRQ eine Quittierung.");
    require(throws_runtime_error(
                [&] { static_cast<void>(controller.read(0xA0u, MemoryAccessWidth::Byte)); }) &&
                throws_runtime_error(
                    [&] { controller.write(0xA0u, 0u, MemoryAccessWidth::Byte); }),
            "Der veraltete GD-ROM-Device-Control-Offset 0xA0 bleibt faelschlich aktiv.");
    controller.reset();

    controller.write(0x90u, 4u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    cpu.r[4] = 16u;
    cpu.r[5] = 0u;
    require(controller.bios_call(cpu, 0u, 0u) == 0u,
            "Ein Taskfile-Paket gibt den exklusiven Laufwerksbesitz an das BIOS ab.");
    std::array<std::uint8_t, 12u> request_mode{};
    request_mode[0] = 0x11u;
    request_mode[4] = 10u;
    write_packet(controller, request_mode);
    require(controller.status().ata_status == 0x80u && completions == 0u &&
                controller.status().completed_commands == 0u,
            "GD-ROM-Paketkommando schliesst synchron beim letzten Paketwort ab.");
    static_cast<void>(scheduler.advance_by(999u, 1u));
    require(completions == 0u, "GD-ROM-Paketcompletion wird vor dem Zielzyklus signalisiert.");
    static_cast<void>(scheduler.advance_by(1u, 1u));
    require(completions == 1u && controller.status().completed_commands == 0u &&
                controller.status().pio_bytes_available == 10u &&
                controller.status().interrupt_reason == 2u &&
                controller.read(0x90u, MemoryAccessWidth::Byte) == 4u &&
                controller.bios_call(cpu, 0u, 0u) == 0u,
            "REQ_MODE publiziert seine erste DataIn-Phase nicht asynchron und exklusiv.");
    require(controller.read(0x18u, MemoryAccessWidth::Byte) == 0x08u &&
                command_acks == 0u &&
                controller.read(0x9Cu, MemoryAccessWidth::Byte) == 0x08u &&
                command_acks == 1u,
            "Alternate- und Command-Status trennen IRQ-Beobachtung und Quittierung nicht.");
    std::vector<std::uint8_t> mode_bytes;
    append_pio_word(controller, mode_bytes);
    append_pio_word(controller, mode_bytes);
    require(completions == 2u && controller.read(0x90u, MemoryAccessWidth::Byte) == 4u &&
                controller.status().pio_bytes_available == 6u &&
                controller.read(0x9Cu, MemoryAccessWidth::Byte) == 0x08u,
            "REQ_MODE beginnt nach vier Bytes keine zweite IRQ-Phase.");
    append_pio_word(controller, mode_bytes);
    append_pio_word(controller, mode_bytes);
    require(completions == 3u && controller.read(0x90u, MemoryAccessWidth::Byte) == 2u &&
                controller.status().pio_bytes_available == 2u &&
                controller.read(0x9Cu, MemoryAccessWidth::Byte) == 0x08u,
            "REQ_MODE dekrementiert ByteCount in der zweiten Phase nicht exakt.");
    append_pio_word(controller, mode_bytes);
    require(mode_bytes.size() == 10u && completions == 4u &&
                controller.status().completed_commands == 1u &&
                controller.status().pio_bytes_available == 0u &&
                controller.status().interrupt_reason == 3u &&
                controller.read(0x90u, MemoryAccessWidth::Byte) == 0u &&
                controller.read(0x9Cu, MemoryAccessWidth::Byte) == 0x40u &&
                command_acks == 4u,
            "REQ_MODE liefert nicht drei DataIn-IRQs plus finalen Status-IRQ.");

    controller.write(0x90u, 32u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    request_mode[2] = 0u;
    request_mode[4] = 32u;
    write_packet(controller, request_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require(controller.status().pio_bytes_available == 32u &&
                controller.read(0x90u, MemoryAccessWidth::Byte) == 32u,
            "REQ_MODE stellt nicht den vollstaendigen 32-Byte-Hardwareinfopuffer bereit.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    std::vector<std::uint8_t> full_mode;
    for (std::size_t index = 0u; index < 16u; ++index)
        append_pio_word(controller, full_mode);
    require(full_mode.size() == 32u && full_mode[2] == 0u && full_mode[5] == 0xB4u &&
                full_mode[6] == 0x19u && full_mode[9] == 0x08u &&
                std::all_of(full_mode.begin() + 10, full_mode.end(), [](const auto value) {
                    return value == 0u;
                }),
            "REQ_MODE liefert keine strukturell vollstaendige, kontrolliert leere Identitaet.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    controller.write(0x90u, 22u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    request_mode[2] = 10u;
    request_mode[4] = 22u;
    write_packet(controller, request_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    std::vector<std::uint8_t> identity_mode;
    for (std::size_t index = 0u; index < 11u; ++index)
        append_pio_word(controller, identity_mode);
    require(identity_mode.size() == 22u &&
                std::all_of(identity_mode.begin(), identity_mode.end(), [](const auto value) {
                    return value == 0u;
                }),
            "REQ_MODE schneidet die herstellerdefinierten Identitaetsfelder bei Byte 10 ab.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    controller.write(0x90u, 2u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    request_mode[2] = 1u;
    request_mode[4] = 2u;
    write_packet(controller, request_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require((controller.status().ata_status & 1u) != 0u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x50u,
            "REQ_MODE akzeptiert eine ungerade Hardwareinfo-Startadresse.");
    const auto odd_mode_sense = read_taskfile_sense(controller, scheduler);
    require(odd_mode_sense[2] == 5u && odd_mode_sense[8] == 0x24u,
            "REQ_MODE meldet eine ungerade Startadresse nicht als INVALID FIELD.");

    controller.reset();
    controller.write(0x90u, 4u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    request_mode[2] = 0u;
    request_mode[4] = 10u;
    const auto unacked_phase_completions = completions;
    const auto unacked_phase_acks = command_acks;
    write_packet(controller, request_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    std::vector<std::uint8_t> unacked_phase;
    append_pio_word(controller, unacked_phase);
    append_pio_word(controller, unacked_phase);
    require(completions == unacked_phase_completions + 1u &&
                controller.read(0x90u, MemoryAccessWidth::Byte) == 4u,
            "Eine neue DataIn-Phase erzeugt ohne Low-Flanke einen doppelten Command-IRQ.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    require(completions == unacked_phase_completions + 2u &&
                command_acks == unacked_phase_acks + 1u,
            "Ein waehrend High wartender Phasen-IRQ wird nach dem Ack nicht neu signalisiert.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    require(command_acks == unacked_phase_acks + 2u,
            "Der nachgezogene Phasen-IRQ laesst sich nicht separat quittieren.");

    controller.reset();
    const auto alternate_completions = completions;
    const auto alternate_acks = command_acks;
    const auto alternate_commands = controller.status().completed_commands;
    std::array<std::uint8_t, 12u> test_unit{};
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    write_packet(controller, test_unit);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require(controller.read(0x18u, MemoryAccessWidth::Byte) == 0x40u &&
                completions == alternate_completions + 1u &&
                command_acks == alternate_acks,
            "Alternate Status veraendert den finalen Command-IRQ.");
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    write_packet(controller, test_unit);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require(controller.status().completed_commands == alternate_commands + 2u &&
                completions == alternate_completions + 1u,
            "Finalstatus haelt Owner/Phase fest oder dupliziert einen bereits hohen IRQ.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    require(completions == alternate_completions + 2u &&
                command_acks == alternate_acks + 2u,
            "Der ohne Command-Status gestartete Folgeauftrag verliert seinen finalen IRQ.");

    constexpr std::uint32_t taskfile_dma_destination = 0x8C030000u;
    controller.reset();
    const auto pio_cd_read_completions = completions;
    const auto pio_cd_read_commands = controller.status().completed_commands;
    issue_cd_read(controller, scheduler, 0u, 150u, 1u, 2048u);
    require(completions == pio_cd_read_completions + 1u &&
                controller.status().completed_commands == pio_cd_read_commands &&
                controller.status().ata_status == 0x08u &&
                controller.status().interrupt_reason == 2u &&
                controller.status().pio_bytes_available == 2048u &&
                throws_runtime_error([&] {
                    controller.dma_to_memory(taskfile_dma_destination, 2048u, 1u);
                }),
            "CD_READ ohne DMA-Feature verlaesst den PIO-DRQ-Vertrag.");

    controller.reset();
    const auto taskfile_dma_completions = completions;
    const auto taskfile_dma_commands = controller.status().completed_commands;
    const auto taskfile_dma_count = controller.status().completed_dma;
    issue_cd_read(controller, scheduler, 1u, 150u, 1u);
    require(completions == taskfile_dma_completions &&
                controller.status().completed_commands == taskfile_dma_commands &&
                controller.status().completed_dma == taskfile_dma_count &&
                controller.status().ata_status == 0x80u &&
                controller.status().interrupt_reason == 0u &&
                controller.status().pio_bytes_available == 0u &&
                throws_runtime_error(
                    [&] { static_cast<void>(controller.read(0x80u, MemoryAccessWidth::Halfword)); }),
            "CD_READ-DMA erzeugt faelschlich eine PIO-DRQ-Phase oder Zwischen-IRQ.");
    controller.dma_to_memory(taskfile_dma_destination, 1024u, 1u);
    require(completions == taskfile_dma_completions &&
                controller.status().ata_status == 0x80u &&
                controller.status().completed_commands == taskfile_dma_commands &&
                controller.status().completed_dma == taskfile_dma_count &&
                cpu.memory.read_u8(taskfile_dma_destination) == 0x5Au,
            "Partielle CD_READ-DMA beendet den Auftrag oder signalisiert einen Zwischen-IRQ.");
    require(throws_out_of_range([&] {
                controller.dma_to_memory(taskfile_dma_destination + 1024u, 1025u, 1u);
            }) &&
                controller.status().ata_status == 0x80u,
            "CD_READ-DMA akzeptiert einen Transfer ueber den verbleibenden Puffer hinaus.");
    controller.dma_to_memory(taskfile_dma_destination + 1024u, 1024u, 1u);
    require(completions == taskfile_dma_completions + 1u &&
                controller.status().completed_commands == taskfile_dma_commands + 1u &&
                controller.status().completed_dma == taskfile_dma_count + 1u &&
                controller.status().ata_status == 0x40u &&
                controller.status().interrupt_reason == 3u &&
                controller.status().pio_bytes_available == 0u &&
                cpu.memory.read_u8(taskfile_dma_destination + 2047u) == 0x5Au,
            "CD_READ-DMA liefert nicht exakt einen finalen Status-IRQ nach DMACK-Datenende.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    std::vector<SystemAsicEvent> taskfile_fault_events;
    std::uint64_t taskfile_fault_handler_calls = 0u;
    DreamcastG1BusController taskfile_fault_g1(
        scheduler,
        HollyDmaTiming{4u},
        [&](const std::uint32_t address,
            const std::uint32_t length,
            const std::uint32_t direction) {
            ++taskfile_fault_handler_calls;
            controller.dma_to_memory(address, length, direction);
        },
        [&](const SystemAsicEvent event) { taskfile_fault_events.push_back(event); });
    std::optional<G1DmaFault> taskfile_fault;
    taskfile_fault_g1.set_fault_observer([&](const G1DmaFault& fault) {
        taskfile_fault = fault;
        controller.handle_g1_dma_fault(fault);
    });
    controller.bind_g1_bus(&taskfile_fault_g1);
    controller.reset();
    taskfile_fault_g1.reset();
    const auto range_fault_completions = completions;
    const auto range_fault_commands = controller.status().completed_commands;
    const auto range_fault_dma = controller.status().completed_dma;
    issue_cd_read(controller, scheduler, 1u, 150u, 1u);
    require(!taskfile_fault_g1.begin_transfer(0x0BFFFFE0u, 2048u, 1u) &&
                taskfile_fault && taskfile_fault->reason == HollyDmaFaultReason::IllegalAddress &&
                taskfile_fault->fault_address == 0x0BFFFFE0u &&
                taskfile_fault->transferred_bytes == 0u && taskfile_fault->residue == 2048u &&
                taskfile_fault->phase == G1DmaFaultPhase::Start &&
                taskfile_fault_handler_calls == 0u &&
                taskfile_fault_events ==
                    std::vector<SystemAsicEvent>{SystemAsicEvent::GdromIllegalAddress} &&
                completions == range_fault_completions + 1u &&
                controller.status().completed_commands == range_fault_commands + 1u &&
                controller.status().completed_dma == range_fault_dma &&
                controller.status().ata_status == 0x41u &&
                controller.status().interrupt_reason == 3u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x54u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x54u &&
                throws_runtime_error([&] {
                    controller.dma_to_memory(taskfile_dma_destination, 2048u, 1u);
                }),
            "Synchroner G1-Rangefehler verlaesst Taskfile-DMA nicht mit exakter Evidenz, "
            "CHECK/ABRT und genau einem finalen Command-IRQ.");
    static_cast<void>(scheduler.advance_by(16'384u, 32u));
    require(completions == range_fault_completions + 1u &&
                taskfile_fault_handler_calls == 0u &&
                taskfile_fault_g1.state().fault_count == 1u,
            "Synchroner G1-Rangefehler laesst einen spaeten Chunk oder IRQ durch.");
    const auto range_fault_sense = read_taskfile_sense(controller, scheduler);
    require(range_fault_sense[2] == 5u && range_fault_sense[8] == 0x21u &&
                range_fault_sense[9] == 0u,
            "Synchroner G1-Rangefehler haelt keinen stabilen Illegal-Address-Sense fest.");

    controller.reset();
    const auto invalid_completions = completions;
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    std::array<std::uint8_t, 12u> invalid{};
    invalid[0] = 0xFFu;
    write_packet(controller, invalid);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require(completions == invalid_completions + 1u &&
                (controller.status().ata_status & 1u) != 0u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x50u &&
                controller.status().interrupt_reason == 3u,
            "Unbekanntes SPI-Kommando wird nicht kontrolliert mit persistentem Sense beendet.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    controller.write(0x90u, 4u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    std::array<std::uint8_t, 12u> request_error{};
    request_error[0] = 0x13u;
    request_error[4] = 10u;
    const auto sense_completions = completions;
    write_packet(controller, request_error);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    std::vector<std::uint8_t> sense;
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    append_pio_word(controller, sense);
    append_pio_word(controller, sense);
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    append_pio_word(controller, sense);
    append_pio_word(controller, sense);
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    append_pio_word(controller, sense);
    require(sense == std::vector<std::uint8_t>({0xF0u, 0u, 5u, 0u, 0u,
                                                0u, 0u, 0u, 0x20u, 0u}) &&
                completions == sense_completions + 4u &&
                (controller.status().ata_status & 1u) == 0u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0u,
            "REQ_ERROR liefert oder loescht den persistenten Sense nicht phasengenau.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    const auto ata_error_completions = completions;
    controller.write(0x9Cu, 0x77u, MemoryAccessWidth::Byte);
    require(completions == ata_error_completions + 1u &&
                (controller.status().ata_status & 1u) != 0u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x54u &&
                controller.status().interrupt_reason == 3u,
            "Unbekanntes ATA-Kommando stuerzt den Host ab oder verliert ABRT/Sense.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    const auto success_after_error_completions = completions;
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    write_packet(controller, test_unit);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require(completions == success_after_error_completions + 1u &&
                controller.status().ata_status == 0x40u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x50u,
            "Erfolgreiches Folgekommando erbt faelschlich ATA ERR vom persistenten Sense.");
    const auto preserved_sense = read_taskfile_sense(controller, scheduler);
    require(preserved_sense[2] == 5u && preserved_sense[8] == 0x20u,
            "Erfolgreiches Folgekommando verliert den getrennt gespeicherten Sense-Payload.");

    controller.reset();
    const auto set_features_completions = completions;
    const auto set_features_commands = controller.status().completed_commands;
    controller.write(0x84u, 0x13u, MemoryAccessWidth::Byte);
    controller.write(0x88u, 0x22u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xEFu, MemoryAccessWidth::Byte);
    require(completions == set_features_completions + 1u &&
                controller.status().completed_commands == set_features_commands + 1u &&
                controller.status().ata_status == 0x40u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0u &&
                controller.status().interrupt_reason == 3u,
            "ATA SET FEATURES lehnt den belegten Dreamcast-DMA-Modus 0x13/0x22 ab.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    controller.reset();
    const auto rejected_features_completions = completions;
    controller.write(0x84u, 0x03u, MemoryAccessWidth::Byte);
    controller.write(0x88u, 0x22u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xEFu, MemoryAccessWidth::Byte);
    require(completions == rejected_features_completions + 1u &&
                (controller.status().ata_status & 1u) != 0u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x54u &&
                controller.status().interrupt_reason == 3u,
            "ATA SET FEATURES akzeptiert einen unbelegten Transfermodus als Scheinerfolg.");
    const auto rejected_features_sense = read_taskfile_sense(controller, scheduler);
    require(rejected_features_sense[2] == 5u && rejected_features_sense[8] == 0x24u,
            "Unbekannter ATA-Transfermodus liefert nicht ABRT mit INVALID FIELD.");

    controller.reset();
    controller.write(0x90u, 4u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    std::array<std::uint8_t, 12u> set_mode{};
    set_mode[0] = 0x12u;
    set_mode[2] = 2u;
    set_mode[4] = 4u;
    write_packet(controller, set_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require(controller.status().interrupt_reason == 0u &&
                controller.read(0x90u, MemoryAccessWidth::Byte) == 4u,
            "SET_MODE startet keine DataOut-Phase mit Host-ByteCount.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    require(throws_runtime_error([&] {
                controller.dma_to_memory(taskfile_dma_destination, 4u, 1u);
            }) &&
                controller.read(0x90u, MemoryAccessWidth::Byte) == 4u,
            "G1-DMA akzeptiert eine DataOut-Phase und kann deren Schreibcursor korrumpieren.");
    controller.write(0x80u, 0x2211u, MemoryAccessWidth::Halfword);
    controller.write(0x80u, 0x4433u, MemoryAccessWidth::Halfword);
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    controller.write(0x90u, 4u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    request_mode[2] = 2u;
    request_mode[4] = 4u;
    write_packet(controller, request_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    std::vector<std::uint8_t> changed_mode;
    append_pio_word(controller, changed_mode);
    append_pio_word(controller, changed_mode);
    require(changed_mode == std::vector<std::uint8_t>({0x11u, 0x22u, 0x33u, 0x44u}),
            "SET_MODE-DataOut wird nicht persistent durch REQ_MODE sichtbar.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    controller.write(0x90u, 2u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    set_mode[2] = 1u;
    set_mode[4] = 2u;
    write_packet(controller, set_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    require((controller.status().ata_status & 1u) != 0u &&
                controller.read(0x84u, MemoryAccessWidth::Byte) == 0x50u,
            "SET_MODE akzeptiert eine ungerade Modepuffer-Startadresse.");
    const auto odd_set_mode_sense = read_taskfile_sense(controller, scheduler);
    require(odd_set_mode_sense[2] == 5u && odd_set_mode_sense[8] == 0x24u,
            "SET_MODE meldet eine ungerade Startadresse nicht als INVALID FIELD.");

    controller.reset();
    issue_cd_read(controller, scheduler, 0u, 150u, 0u);
    require((controller.status().ata_status & 1u) != 0u,
            "CD_READ mit leerer Sektoranzahl wird nicht abgelehnt.");
    const auto invalid_field_sense = read_taskfile_sense(controller, scheduler);
    require(invalid_field_sense[2] == 5u && invalid_field_sense[8] == 0x24u,
            "GD-ROM InvalidField wird nicht auf Sense ASC 24 abgebildet.");

    controller.reset();
    issue_cd_read(controller, scheduler, 3u, 150u, 1u);
    require((controller.status().ata_status & 1u) != 0u &&
                throws_runtime_error([&] {
                    controller.dma_to_memory(taskfile_dma_destination, 2048u, 1u);
                }),
            "CD_READ aktiviert DMA trotz reservierter Featurebits.");
    const auto invalid_feature_sense = read_taskfile_sense(controller, scheduler);
    require(invalid_feature_sense[2] == 5u && invalid_feature_sense[8] == 0x24u,
            "Reservierte CD_READ-Featurebits liefern nicht INVALID FIELD.");

    controller.reset();
    issue_cd_read(controller, scheduler, 0u, 158u, 1u);
    require((controller.status().ata_status & 1u) != 0u,
            "CD_READ ausserhalb der Disc wird nicht abgelehnt.");
    const auto out_of_range_sense = read_taskfile_sense(controller, scheduler);
    require(out_of_range_sense[2] == 5u && out_of_range_sense[8] == 0x21u,
            "GD-ROM OutOfRange wird nicht auf Sense ASC 21 abgebildet.");

    EventScheduler no_media_scheduler;
    auto no_media_source = std::make_shared<MemoryDiscSource>(
        std::span<const std::uint8_t>{}, "synthetic-no-media");
    DreamcastGdRomController no_media_controller(
        cpu.memory,
        no_media_scheduler,
        GdRomDrive(std::move(no_media_source)),
        {},
        {},
        {},
        {},
        {},
        DiscLoadExecutionPolicy::StandaloneTestMode);
    no_media_controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    write_packet(no_media_controller, test_unit);
    static_cast<void>(no_media_scheduler.advance_by(1'000u, 1u));
    require((no_media_controller.status().ata_status & 1u) != 0u,
            "TEST UNIT READY meldet ein leeres Medium als bereit.");
    const auto no_media_sense = read_taskfile_sense(no_media_controller, no_media_scheduler);
    require(no_media_sense[2] == 2u && no_media_sense[8] == 0x3Au,
            "GD-ROM NoMedia wird nicht auf Sense ASC 3A abgebildet.");

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
    const auto queued_status = controller.status().ata_status;
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    cpu.r[4] = read_request;
    cpu.r[5] = extended_status;
    require(read_request >= 1u && queued_status == 0x80u &&
                controller.status().bios_requests == 1u &&
                controller.status().ata_status == queued_status &&
                controller.status().interrupt_reason == 0u &&
                controller.bios_call(cpu, 1u, 0u) == 1u,
            "REQ_CMD oder DriveOwner zeigt den exklusiven BIOS-Request nicht als BSY.");
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    require(controller.bios_call(cpu, 1u, 0u) == 1u &&
                cpu.memory.read_u32(extended_status + 12u) == 4u &&
                controller.status().ata_status == 0x80u,
            "EXEC_SERVER meldet waehrend BSY keinen erweiterten WAIT-Zustand 4.");
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

    const auto first_bios_completion_count = completions;
    cpu.memory.write_u32(parameters + 8u, destination + 2048u);
    cpu.r[4] = 16u;
    cpu.r[5] = parameters;
    const auto second_read_request = controller.bios_call(cpu, 0u, 0u);
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    static_cast<void>(scheduler.advance_by(1'500u, 1u));
    cpu.r[4] = second_read_request;
    cpu.r[5] = extended_status;
    require(second_read_request >= 1u && completions == first_bios_completion_count + 1u &&
                controller.bios_call(cpu, 1u, 0u) == 2u &&
                cpu.memory.read_u8(destination + 2048u) == 0x5Au,
            "Zweite BIOS-Completion verliert ohne Taskfile-STATUS-Read ihre IRQ-Flanke.");

    cpu.memory.write_u32(0x8CFFFFFCu, 0xA5A55A5Au);
    const auto invalid_destination_completions = completions;
    for (const auto rejected_destination :
         std::array<std::uint32_t, 3u>{0x8CFFFF00u, 0x01000000u, 0xA05F0000u}) {
        cpu.memory.write_u32(parameters + 8u, rejected_destination);
        cpu.r[4] = 16u;
        cpu.r[5] = parameters;
        const auto rejected_request = controller.bios_call(cpu, 0u, 0u);
        static_cast<void>(controller.bios_call(cpu, 2u, 0u));
        cpu.r[4] = rejected_request;
        cpu.r[5] = extended_status;
        require(rejected_request >= 1u && controller.bios_call(cpu, 1u, 0u) == 0xFFFFFFFFu &&
                    cpu.memory.read_u32(extended_status) == 5u &&
                    cpu.memory.read_u32(extended_status + 4u) ==
                        static_cast<std::uint32_t>(GdRomStatus::InvalidField),
                "Ungueltiger BIOS-Lesepuffer wird nicht kontrolliert als INVALID FIELD beendet.");
    }
    require(completions == invalid_destination_completions && rejected_mmio_writes == 0u &&
                cpu.memory.read_u32(0x8CFFFFFCu) == 0xA5A55A5Au,
            "Abgelehnter BIOS-Lesepuffer schreibt partiell, greift MMIO an oder erzeugt einen IRQ.");
    cpu.memory.write_u32(parameters + 8u, destination);

    const auto commands_before_nop = controller.status().completed_commands;
    cpu.r[4] = 29u;
    cpu.r[5] = 0u;
    const auto no_operation = controller.bios_call(cpu, 0u, 0u);
    cpu.r[4] = no_operation;
    cpu.r[5] = extended_status;
    require(no_operation >= 1u && controller.bios_call(cpu, 1u, 0u) == 1u,
            "BIOS CD_CMD_NOP wird nicht zunaechst als PROCESSING sichtbar.");
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    cpu.r[4] = no_operation;
    cpu.r[5] = extended_status;
    require(controller.bios_call(cpu, 1u, 0u) == 2u &&
                cpu.memory.read_u32(extended_status) == 0u &&
                cpu.memory.read_u32(extended_status + 4u) == 0u &&
                cpu.memory.read_u32(extended_status + 8u) == 0u &&
                cpu.memory.read_u32(extended_status + 12u) == 0u &&
                controller.status().completed_commands == commands_before_nop + 1u &&
                controller.bios_call(cpu, 1u, 0u) == 0u,
            "BIOS CD_CMD_NOP schliesst nicht genau einmal als erfolgreiches Kommando ab.");

    constexpr std::uint32_t mode_output = 0x8C006000u;
    cpu.memory.write_u32(parameters, mode_output);
    cpu.memory.write_u32(parameters + 4u, 0u);
    cpu.memory.write_u32(parameters + 8u, 0u);
    cpu.memory.write_u32(parameters + 12u, 0u);
    cpu.r[4] = 30u;
    cpu.r[5] = parameters;
    const auto request_mode_id = controller.bios_call(cpu, 0u, 0u);
    cpu.r[4] = request_mode_id;
    cpu.r[5] = extended_status;
    require(request_mode_id >= 1u && controller.bios_call(cpu, 1u, 0u) == 1u,
            "BIOS REQ_MODE wird nicht zunaechst als PROCESSING sichtbar.");
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    cpu.r[4] = request_mode_id;
    cpu.r[5] = extended_status;
    require(controller.bios_call(cpu, 1u, 0u) == 2u &&
                cpu.memory.read_u32(extended_status) == 0u &&
                cpu.memory.read_u32(extended_status + 4u) == 0u &&
                cpu.memory.read_u32(extended_status + 8u) == 10u &&
                cpu.memory.read_u32(extended_status + 12u) == 0u &&
                cpu.memory.read_u32(mode_output) == 0u &&
                cpu.memory.read_u32(mode_output + 4u) == 0x00B4u &&
                cpu.memory.read_u32(mode_output + 8u) == 0x19u &&
                cpu.memory.read_u32(mode_output + 12u) == 0x08u &&
                controller.bios_call(cpu, 1u, 0u) == 0u,
            "BIOS REQ_MODE liefert weder den Vierwort-Modus noch den einmaligen Abschluss.");

    cpu.memory.write_u32(parameters, 0xA5u);
    cpu.memory.write_u32(parameters + 4u, 0x3456u);
    cpu.memory.write_u32(parameters + 8u, 0x7Cu);
    cpu.memory.write_u32(parameters + 12u, 0x11u);
    cpu.r[4] = 31u;
    cpu.r[5] = parameters;
    const auto set_mode_id = controller.bios_call(cpu, 0u, 0u);
    cpu.r[4] = set_mode_id;
    cpu.r[5] = extended_status;
    require(set_mode_id >= 1u && controller.bios_call(cpu, 1u, 0u) == 1u,
            "BIOS SET_MODE wird nicht zunaechst als PROCESSING sichtbar.");
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    cpu.r[4] = set_mode_id;
    cpu.r[5] = extended_status;
    require(controller.bios_call(cpu, 1u, 0u) == 2u &&
                cpu.memory.read_u32(extended_status) == 0u &&
                cpu.memory.read_u32(extended_status + 4u) == 0u &&
                cpu.memory.read_u32(extended_status + 8u) == 10u &&
                cpu.memory.read_u32(extended_status + 12u) == 0u &&
                controller.bios_call(cpu, 1u, 0u) == 0u,
            "BIOS SET_MODE schliesst nicht mit dem Vierwortstatus und NOT_FOUND ab.");

    controller.write(0x90u, 10u, MemoryAccessWidth::Byte);
    controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    std::array<std::uint8_t, 12u> bios_shared_request_mode{};
    bios_shared_request_mode[0] = 0x11u;
    bios_shared_request_mode[4] = 10u;
    write_packet(controller, bios_shared_request_mode);
    static_cast<void>(scheduler.advance_by(1'000u, 1u));
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));
    std::vector<std::uint8_t> bios_shared_mode;
    for (std::size_t index = 0u; index < 5u; ++index)
        append_pio_word(controller, bios_shared_mode);
    require(bios_shared_mode ==
                std::vector<std::uint8_t>({0u, 0u, 0xA5u, 0u, 0x34u,
                                           0x56u, 0x7Cu, 0u, 0u, 0x11u}),
            "BIOS SET_MODE wird nicht durch den gemeinsamen Paket-REQ_MODE sichtbar.");
    static_cast<void>(controller.read(0x9Cu, MemoryAccessWidth::Byte));

    cpu.memory.write_u32(mode_output, 0u);
    cpu.memory.write_u32(mode_output + 4u, 0u);
    cpu.memory.write_u32(mode_output + 8u, 0u);
    cpu.memory.write_u32(mode_output + 12u, 0u);
    cpu.memory.write_u32(parameters, mode_output);
    cpu.r[4] = 30u;
    cpu.r[5] = parameters;
    const auto persisted_mode = controller.bios_call(cpu, 0u, 0u);
    static_cast<void>(controller.bios_call(cpu, 2u, 0u));
    cpu.r[4] = persisted_mode;
    cpu.r[5] = extended_status;
    require(persisted_mode >= 1u && controller.bios_call(cpu, 1u, 0u) == 2u &&
                cpu.memory.read_u32(mode_output) == 0xA5u &&
                cpu.memory.read_u32(mode_output + 4u) == 0x3456u &&
                cpu.memory.read_u32(mode_output + 8u) == 0x7Cu &&
                cpu.memory.read_u32(mode_output + 12u) == 0x11u,
            "BIOS REQ_MODE sieht den gemeinsam gesetzten Laufwerksmodus nicht.");

    constexpr std::uint32_t partial_mode_output = 0x8CFFFFF8u;
    cpu.memory.write_u32(partial_mode_output, 0x13579BDFu);
    cpu.memory.write_u32(partial_mode_output + 4u, 0x2468ACE0u);
    const auto rejected_mode_mmio_writes = rejected_mmio_writes;
    for (const auto rejected_destination :
         std::array<std::uint32_t, 2u>{partial_mode_output, 0xA05F0000u}) {
        cpu.memory.write_u32(parameters, rejected_destination);
        cpu.r[4] = 30u;
        cpu.r[5] = parameters;
        const auto rejected_mode = controller.bios_call(cpu, 0u, 0u);
        static_cast<void>(controller.bios_call(cpu, 2u, 0u));
        cpu.r[4] = rejected_mode;
        cpu.r[5] = extended_status;
        require(rejected_mode >= 1u &&
                    controller.bios_call(cpu, 1u, 0u) == 0xFFFFFFFFu &&
                    cpu.memory.read_u32(extended_status) == 5u &&
                    cpu.memory.read_u32(extended_status + 4u) ==
                        static_cast<std::uint32_t>(GdRomStatus::InvalidField) &&
                    controller.bios_call(cpu, 1u, 0u) == 0u,
                "BIOS REQ_MODE lehnt ein ungueltiges Ziel nicht atomar als INVALID FIELD ab.");
    }
    require(cpu.memory.read_u32(partial_mode_output) == 0x13579BDFu &&
                cpu.memory.read_u32(partial_mode_output + 4u) == 0x2468ACE0u &&
                rejected_mmio_writes == rejected_mode_mmio_writes,
            "Abgelehntes BIOS REQ_MODE schreibt partiell oder beruehrt MMIO.");

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

    for (const auto command : std::array<std::uint32_t, 2u>{38u, 39u}) {
        for (const auto p2 :
             std::array<std::uint32_t, 3u>{0u, 1u, std::numeric_limits<std::uint32_t>::max()}) {
            const auto before = controller.status();
            cpu.memory.write_u32(parameters, 150u);
            cpu.memory.write_u32(parameters + 4u, 1u);
            cpu.memory.write_u32(parameters + 8u, p2);
            cpu.memory.write_u32(parameters + 12u, 0xA55AA55Au);
            cpu.r[4] = command;
            cpu.r[5] = parameters;
            const auto ex_request = controller.bios_call(cpu, 0u, 0u);
            static_cast<void>(controller.bios_call(cpu, 2u, 0u));
            cpu.r[4] = ex_request;
            cpu.r[5] = extended_status;
            require(ex_request >= 1u &&
                        controller.bios_call(cpu, 1u, 0u) == 0xFFFFFFFFu &&
                        cpu.memory.read_u32(extended_status) == 5u &&
                        cpu.memory.read_u32(extended_status + 4u) ==
                            static_cast<std::uint32_t>(GdRomStatus::InvalidCommand) &&
                        cpu.memory.read_u32(extended_status + 8u) == 0u &&
                        cpu.memory.read_u32(extended_status + 12u) == 0u &&
                        controller.last_bios_request().id == ex_request &&
                        controller.last_bios_request().command == command &&
                        controller.last_bios_request().state == GdRomBiosRequestState::Error &&
                        controller.bios_call(cpu, 1u, 0u) == 0u,
                    "BIOS-Streaming-EX wird nicht kontrolliert als Illegal Request abgelehnt.");
            const auto after = controller.status();
            require(after.bios_requests == before.bios_requests &&
                        after.completed_commands == before.completed_commands &&
                        after.completed_dma == before.completed_dma &&
                        after.sector_mode == before.sector_mode &&
                        after.dma_callback == before.dma_callback &&
                        after.dma_callback_argument == before.dma_callback_argument &&
                        after.pio_callback == before.pio_callback &&
                        after.pio_callback_argument == before.pio_callback_argument &&
                        after.stream_bytes_remaining == before.stream_bytes_remaining &&
                        after.transfer_bytes_remaining == before.transfer_bytes_remaining &&
                        after.pending_guest_callbacks == before.pending_guest_callbacks,
                    "Abgelehntes BIOS-Streaming-EX mutiert Streaming- oder Callbackzustand.");
        }
    }

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

    const auto stream_cpu_storage = std::make_unique<CpuState>();
    auto& stream_cpu = *stream_cpu_storage;
    stream_cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(stream_cpu.memory));
    EventScheduler stream_scheduler;
    std::vector<std::uint8_t> stream_bytes(4u * 2048u);
    for (std::size_t index = 0u; index < stream_bytes.size(); ++index)
        stream_bytes[index] = static_cast<std::uint8_t>((index * 73u + 0x31u) & 0xFFu);
    auto stream_source =
        std::make_shared<MemoryDiscSource>(stream_bytes, "synthetic-streaming-gdrom");
    std::uint64_t stream_drive_completions = 0u;
    bool observe_stream_module_loads = false;
    std::uint64_t stream_module_sequence = 0u;
    std::vector<std::pair<std::uint32_t, std::size_t>> stream_module_loads;
    ExecutableLoadWriteTracker stream_load_writes;
    ExecutableModuleCatalog stream_modules;
    RuntimeBlockTable stream_blocks;
    ExecutableCodeTracker stream_code_tracker;
    stream_blocks.bind_code_tracker(&stream_code_tracker);
    DreamcastGdRomController stream_controller(
        stream_cpu.memory,
        stream_scheduler,
        GdRomDrive(std::move(stream_source)),
        [&](const std::uint64_t) { ++stream_drive_completions; },
        [&](const std::uint32_t physical,
            const std::span<const std::uint8_t> loaded,
            const std::string_view source_identity) {
            if (!observe_stream_module_loads) return;
            stream_module_loads.emplace_back(physical, loaded.size());
            ExecutableModule module;
            module.id = "stream-module-" + std::to_string(stream_module_sequence++);
            module.source_identity = std::string(source_identity);
            module.guest_start = physical;
            module.bytes.assign(loaded.begin(), loaded.end());
            module.kind = ExecutableModuleKind::Overlay;
            module.executable_permission = false;
            module.control_transfer_promotion_allowed = true;
            module.range_roles.push_back(
                {0u,
                 static_cast<std::uint32_t>(loaded.size()),
                 ExecutableStorageRole::ProvenData});
            stream_modules.publish_loaded_range(
                std::move(module),
                stream_blocks,
                stream_code_tracker,
                stream_load_writes.consume(physical, loaded.size()));
        },
        {},
        {},
        {},
        DiscLoadExecutionPolicy::StandaloneTestMode);
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
        const auto queued_state = stream_controller.bios_call(stream_cpu, 1u, 0u);
        const auto queued_error =
            "Streaming-REQ_CMD wird nicht als PROCESSING eingereiht: command=" +
            std::to_string(command) + ", fad=" + std::to_string(fad) +
            ", request=" + std::to_string(request) +
            ", state=" + std::to_string(queued_state);
        require(request >= 1u && queued_state == 1u,
                queued_error.c_str());
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
        const auto ready_result = stream_controller.bios_call(stream_cpu, 1u, 0u);
        require(stream_drive_completions == completions_before_ready + 1u,
                "Streaming-Completion erzeugt keine neue Command-IRQ-Flanke.");
        require(ready_result == 3u &&
                    stream_cpu.memory.read_u32(stream_status + 8u) == 0u &&
                    stream_controller.status().stream_bytes_remaining ==
                        static_cast<std::uint64_t>(sector_count) * 2048u,
                "Streaming-Request erreicht nach der Gastzeit keinen STREAMING-Zustand.");
        static_cast<void>(stream_controller.read(0x9Cu, MemoryAccessWidth::Byte));
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
    stream_cpu.r[5] = stream_status;
    require(stream_controller.bios_call(stream_cpu, 1u, 0u) == 3u &&
                stream_cpu.memory.read_u32(stream_status + 12u) == 4u &&
                stream_controller.status().ata_status == 0x80u,
            "Aktives BIOS-DMA-Streaming meldet waehrend BSY keinen WAIT-Zustand 4.");
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
                stream_g1.state().system_counter == 0x0C040800u &&
                stream_g1.state().peripheral_counter == 2048u &&
                stream_controller.bios_call(stream_cpu, 7u, 0u) == 1u &&
                stream_cpu.memory.read_u32(stream_residue) == 2048u &&
                stream_controller.status().stream_bytes_remaining == 2048u,
            "Erster BIOS-DMA-Streamingchunk aktualisiert Daten oder Residue nicht exakt.");
    static_cast<void>(stream_scheduler.advance_by(8192u, 1u));
    require(stream_memory_matches(dma_stream_destination, 0u, 4096u) &&
                stream_g1.state().active == 0u && stream_g1.state().remaining == 0u &&
                stream_g1.state().system_counter == 0x0C041000u &&
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

    std::uint32_t faulting_stream_handler_calls = 0u;
    std::vector<SystemAsicEvent> faulting_stream_events;
    DreamcastG1BusController faulting_stream_g1(
        stream_scheduler,
        HollyDmaTiming{4u},
        [&](const std::uint32_t address,
            const std::uint32_t length,
            const std::uint32_t direction) {
            if (faulting_stream_handler_calls++ != 0u)
                throw std::runtime_error("synthetic second G1 chunk failure");
            stream_controller.dma_to_memory(address, length, direction);
        },
        [&](const SystemAsicEvent event) { faulting_stream_events.push_back(event); });
    std::optional<G1DmaFault> faulting_stream_fault;
    faulting_stream_g1.set_fault_observer([&](const G1DmaFault& fault) {
        faulting_stream_fault = fault;
        stream_controller.handle_g1_dma_fault(fault);
    });
    stream_controller.bind_g1_bus(&faulting_stream_g1);
    constexpr std::uint32_t faulting_stream_destination = 0x8C043000u;
    stream_cpu.memory.write_bytes(faulting_stream_destination, untouched_stream);
    const auto faulting_stream_request = queue_ready_stream(28u, 152u, 2u);
    const auto drive_completions_before_g1_fault = stream_drive_completions;
    const auto completed_dma_before_g1_fault = stream_controller.status().completed_dma;
    stream_cpu.memory.write_u32(stream_transfer, faulting_stream_destination);
    stream_cpu.memory.write_u32(stream_transfer + 4u, 4096u);
    stream_cpu.r[4] = faulting_stream_request;
    stream_cpu.r[5] = stream_transfer;
    require(stream_controller.bios_call(stream_cpu, 6u, 0u) == 0u,
            "BIOS-G1-Faultregression kann den DMA-Streamingtransfer nicht starten.");
    static_cast<void>(stream_scheduler.advance_by(8192u, 1u));
    require(faulting_stream_handler_calls == 1u &&
                stream_memory_matches(faulting_stream_destination, 4096u, 2048u) &&
                stream_cpu.memory.read_u8(faulting_stream_destination + 2048u) == 0xCCu,
            "BIOS-G1-Faultregression erreicht keinen exakt committed ersten Chunk.");
    static_cast<void>(stream_scheduler.advance_by(8192u, 1u));
    const auto& faulting_stream_status = stream_controller.last_bios_request();
    require(faulting_stream_handler_calls == 2u && faulting_stream_fault &&
                faulting_stream_fault->reason == HollyDmaFaultReason::TransferFailure &&
                faulting_stream_fault->fault_address == 0x0C043800u &&
                faulting_stream_fault->transferred_bytes == 2048u &&
                faulting_stream_fault->residue == 2048u &&
                faulting_stream_fault->phase == G1DmaFaultPhase::Chunk &&
                faulting_stream_g1.state().active == 0u &&
                !faulting_stream_g1.state().completion_event && faulting_stream_events.empty() &&
                stream_memory_matches(faulting_stream_destination, 4096u, 2048u) &&
                stream_cpu.memory.read_u8(faulting_stream_destination + 2048u) == 0xCCu &&
                faulting_stream_status.id == faulting_stream_request &&
                faulting_stream_status.state == GdRomBiosRequestState::Error &&
                faulting_stream_status.status[2] == 2048u &&
                stream_controller.status().stream_bytes_remaining == 2048u &&
                stream_controller.status().completed_dma == completed_dma_before_g1_fault &&
                stream_drive_completions == drive_completions_before_g1_fault,
            "Asynchroner G1-Chunkfehler verliert Prefix, Residue, Fehlerzustand oder erzeugt "
            "eine falsche Completion.");
    static_cast<void>(stream_scheduler.advance_by(32'768u, 64u));
    stream_cpu.r[4] = dma_callback_address;
    stream_cpu.r[5] = dma_callback_argument;
    require(faulting_stream_handler_calls == 2u && faulting_stream_events.empty() &&
                stream_controller.bios_call(stream_cpu, 5u, 0u) == 0xFFFFFFFFu &&
                !stream_controller.take_pending_guest_callback() &&
                stream_controller.status().pending_guest_callbacks == 0u &&
                stream_drive_completions == drive_completions_before_g1_fault,
            "Fehlgeschlagenes BIOS-DMA-Streaming laesst ein spaetes Event oder Callback durch.");
    stream_cpu.r[4] = faulting_stream_request;
    stream_cpu.r[5] = stream_status;
    require(stream_controller.bios_call(stream_cpu, 1u, 0u) == 0xFFFFFFFFu &&
                stream_cpu.memory.read_u32(stream_status) == 0x0Bu &&
                stream_cpu.memory.read_u32(stream_status + 4u) ==
                    static_cast<std::uint32_t>(GdRomStatus::Aborted) &&
                stream_cpu.memory.read_u32(stream_status + 8u) == 2048u &&
                stream_cpu.memory.read_u32(stream_status + 12u) == 0u &&
                stream_controller.bios_call(stream_cpu, 1u, 0u) == 0u,
            "Fehlgeschlagenes BIOS-DMA-Streaming liefert keinen einmaligen exakten Errorstatus.");
    stream_controller.bind_g1_bus(&stream_g1);

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
    stream_cpu.r[4] = pio_stream_request;
    stream_cpu.r[5] = stream_status;
    require(stream_controller.bios_call(stream_cpu, 1u, 0u) == 0u,
            "Abgerufener PIO-Streamingstatus blockiert den naechsten Request.");

    observe_stream_module_loads = true;
    stream_cpu.memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        stream_load_writes.observe(event);
        stream_modules.record_runtime_write(
            event.address, event.size, event.source, event.bytes_changed);
        const auto invalidation = stream_code_tracker.observe_write(
            event.address, event.size, event.source, event.bytes_changed);
        if (!invalidation.byte_identical)
            static_cast<void>(
                stream_blocks.erase_overlapping_physical(event.address, event.size));
    });
    stream_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
    stream_cpu.write_sr(sr_md_mask);
    stream_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
    stream_cpu.address_space->write_mmucr(1u);
    stream_cpu.address_space->ldtlb(
        {0x00002000u, 0x0C050000u, 4096u, 0u, 0u, true, true, true, true, true, true, false});
    stream_cpu.address_space->ldtlb(
        {0x00003000u, 0x0C051000u, 4096u, 0u, 1u, true, true, true, true, true, true, false});
    constexpr std::uint32_t mmu_pio_destination = 0x00002C00u;
    constexpr std::uint32_t mmu_pio_physical = 0x0C050C00u;
    RuntimeBlock mmu_aot;
    mmu_aot.virtual_start = mmu_pio_destination;
    mmu_aot.physical_origin = mmu_pio_physical;
    mmu_aot.size = 2u;
    mmu_aot.end_kind = BlockEndKind::Return;
    mmu_aot.function = dormant_aot_block;
    mmu_aot.provenance = "free-mmu-pio-aot-v1";
    const auto mmu_aot_identity = stable_runtime_block_identity(mmu_aot);
    const auto mmu_aot_handle = stream_blocks.register_static(mmu_aot);
    require(stream_code_tracker.register_block({mmu_aot_identity,
                                                mmu_pio_physical,
                                                2u,
                                                mmu_aot.provenance,
                                                {},
                                                ExecutableBlockOrigin::ImageSegment}) ==
                BlockRegistrationResult::Inserted,
            "MMU-PIO-AOT-Block wurde nicht registriert.");
    const auto mmu_pio_request = queue_ready_stream(37u, 150u);
    stream_cpu.memory.write_u32(stream_transfer, mmu_pio_destination);
    stream_cpu.memory.write_u32(stream_transfer + 4u, 2048u);
    stream_cpu.r[4] = mmu_pio_request;
    stream_cpu.r[5] = stream_transfer;
    const auto mmu_pio_result = stream_controller.bios_call(stream_cpu, 12u, 0u);
    const auto mmu_memory_matches = stream_memory_matches(mmu_pio_physical, 0u, 2048u);
    const auto mmu_load_matches =
        stream_module_loads ==
        std::vector<std::pair<std::uint32_t, std::size_t>>{{mmu_pio_physical, 2048u}};
    const auto* mmu_physical_module = stream_modules.resolve(mmu_pio_physical, 2048u);
    const auto* mmu_virtual_module = stream_modules.resolve(mmu_pio_destination, 2u);
    const auto mmu_aot_active = stream_blocks.active(mmu_aot_handle);
    const auto mmu_aot_valid = stream_code_tracker.valid(mmu_aot_identity);
    const auto mmu_invalidations = stream_code_tracker.invalidation_count();
    const auto mmu_physical_generation =
        stream_code_tracker.page_generation(mmu_pio_physical);
    const auto mmu_virtual_generation =
        stream_code_tracker.page_generation(mmu_pio_destination);
    const auto mmu_error =
        "MMU-PIO-Load verwendet nicht durchgehend die committed physische Range: result=" +
        std::to_string(mmu_pio_result) + ", memory=" + std::to_string(mmu_memory_matches) +
        ", load=" + std::to_string(mmu_load_matches) +
        ", physical_module=" + std::to_string(mmu_physical_module != nullptr) +
        ", virtual_module=" + std::to_string(mmu_virtual_module != nullptr) +
        ", active=" + std::to_string(mmu_aot_active) +
        ", valid=" + std::to_string(mmu_aot_valid) +
        ", invalidations=" + std::to_string(mmu_invalidations) +
        ", physical_generation=" + std::to_string(mmu_physical_generation) +
        ", virtual_generation=" + std::to_string(mmu_virtual_generation);
    require(mmu_pio_result == 0u && mmu_memory_matches && mmu_load_matches &&
                mmu_physical_module != nullptr && mmu_virtual_module == nullptr &&
                !mmu_aot_active && !mmu_aot_valid && mmu_invalidations == 1u &&
                mmu_physical_generation != 0u && mmu_virtual_generation == 0u,
            mmu_error.c_str());
    stream_cpu.r[4] = mmu_pio_request;
    stream_cpu.r[5] = stream_status;
    require(stream_controller.bios_call(stream_cpu, 1u, 0u) == 2u &&
                stream_controller.bios_call(stream_cpu, 1u, 0u) == 0u,
            "Abgerufener MMU-PIO-Status blockiert den Nichtlinearitaetstest.");

    stream_cpu.address_space->ldtlb(
        {0x00004000u, 0x0C060000u, 4096u, 0u, 2u, true, true, true, true, true, true, false});
    stream_cpu.address_space->ldtlb(
        {0x00005000u, 0x0C062000u, 4096u, 0u, 3u, true, true, true, true, true, true, false});
    constexpr std::uint32_t nonlinear_pio_destination = 0x00004C00u;
    constexpr std::uint32_t nonlinear_first_physical = 0x0C060C00u;
    constexpr std::uint32_t nonlinear_second_physical = 0x0C062000u;
    const auto nonlinear_request = queue_ready_stream(37u, 151u);
    stream_cpu.memory.write_u32(stream_transfer, nonlinear_pio_destination);
    stream_cpu.memory.write_u32(stream_transfer + 4u, 2048u);
    const auto module_loads_before_nonlinear = stream_module_loads.size();
    stream_cpu.r[4] = nonlinear_request;
    stream_cpu.r[5] = stream_transfer;
    require(stream_controller.bios_call(stream_cpu, 12u, 0u) == 0xFFFFFFFFu &&
                stream_cpu.memory.read_u8(nonlinear_first_physical) == 0u &&
                stream_cpu.memory.read_u8(nonlinear_second_physical) == 0u &&
                stream_module_loads.size() == module_loads_before_nonlinear &&
                stream_controller.status().transfer_bytes_remaining == 0u,
            "Nichtlineare MMU-PIO-Range schreibt partiell oder erzeugt einen Modulnachweis.");
    stream_cpu.r[4] = nonlinear_request;
    static_cast<void>(stream_controller.bios_call(stream_cpu, 3u, 0u));
    observe_stream_module_loads = false;
    stream_cpu.memory.set_guest_write_observer({});
    stream_cpu.address_space.reset();
    stream_cpu.write_sr(0u);

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

    const auto toc_cpu_storage = std::make_unique<CpuState>();
    auto& toc_cpu = *toc_cpu_storage;
    toc_cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(toc_cpu.memory));
    EventScheduler toc_scheduler;
    auto toc_source = std::make_shared<LayoutDiscSource>(
        std::vector<DiscTrackLayout>{{1u, 0u, DiscTrackKind::Audio, 2352u, 100u, 1u},
                                     {2u, 100u, DiscTrackKind::Audio, 2352u, 100u, 1u},
                                     {3u, 45'000u, DiscTrackKind::Data, 2048u, 100u, 2u}},
        "synthetic-two-area-gdrom");
    std::uint64_t toc_completions = 0u;
    DreamcastGdRomController toc_controller(
        toc_cpu.memory,
        toc_scheduler,
        GdRomDrive(std::move(toc_source)),
        [&](const std::uint64_t) { ++toc_completions; },
        {},
        {},
        {},
        {},
        DiscLoadExecutionPolicy::StandaloneTestMode);
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

    toc_cpu.memory.write_u32(0x8CFFFFFCu, 0x11223344u);
    toc_cpu.memory.write_u32(parameters, 0u);
    toc_cpu.memory.write_u32(parameters + 4u, 0x8CFFFF00u);
    toc_cpu.r[4] = 19u;
    toc_cpu.r[5] = parameters;
    const auto rejected_toc = toc_controller.bios_call(toc_cpu, 0u, 0u);
    static_cast<void>(toc_controller.bios_call(toc_cpu, 2u, 0u));
    toc_cpu.r[4] = rejected_toc;
    toc_cpu.r[5] = extended_status;
    require(rejected_toc >= 1u &&
                toc_controller.bios_call(toc_cpu, 1u, 0u) == 0xFFFFFFFFu &&
                toc_cpu.memory.read_u32(extended_status) == 5u &&
                toc_cpu.memory.read_u32(extended_status + 4u) ==
                    static_cast<std::uint32_t>(GdRomStatus::InvalidField) &&
                toc_cpu.memory.read_u32(0x8CFFFFFCu) == 0x11223344u,
            "Abgelehnter BIOS-TOC-Puffer wird partiell beschrieben oder wirft zum Host.");

    toc_controller.write(0x90u, 64u, MemoryAccessWidth::Byte);
    toc_controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
    toc_controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
    std::array<std::uint8_t, 12u> get_toc{};
    get_toc[0] = 0x14u;
    get_toc[1] = 0u;
    get_toc[3] = 0x01u;
    get_toc[4] = 0x98u;
    write_packet(toc_controller, get_toc);
    static_cast<void>(toc_scheduler.advance_by(1'000u, 1u));
    std::vector<std::uint8_t> spi_toc;
    while (toc_controller.status().pio_bytes_available != 0u) {
        const auto phase_bytes = toc_controller.read(0x90u, MemoryAccessWidth::Byte) |
                                 (toc_controller.read(0x94u, MemoryAccessWidth::Byte) << 8u);
        require(phase_bytes != 0u && (phase_bytes & 1u) == 0u,
                "GET_TOC liefert eine ungueltige PIO-Phasengroesse.");
        static_cast<void>(toc_controller.read(0x9Cu, MemoryAccessWidth::Byte));
        for (std::uint32_t offset = 0u; offset < phase_bytes; offset += 2u)
            append_pio_word(toc_controller, spi_toc);
    }
    static_cast<void>(toc_controller.read(0x9Cu, MemoryAccessWidth::Byte));
    auto spi_toc_matches_bios = spi_toc.size() == 408u;
    for (std::size_t index = 0u; spi_toc_matches_bios && index < 102u; ++index) {
        const auto offset = index * 4u;
        const auto word = static_cast<std::uint32_t>(spi_toc[offset]) |
                          (static_cast<std::uint32_t>(spi_toc[offset + 1u]) << 8u) |
                          (static_cast<std::uint32_t>(spi_toc[offset + 2u]) << 16u) |
                          (static_cast<std::uint32_t>(spi_toc[offset + 3u]) << 24u);
        spi_toc_matches_bios = word == toc_cpu.memory.read_u32(
                                            low_toc + static_cast<std::uint32_t>(offset));
    }
    require(spi_toc_matches_bios && toc_completions == 8u,
            "GET_TOC liefert nicht 102 Gastwoerter in sieben DataIn-Phasen plus Final-IRQ.");

    const auto read_req_stat = [&](const std::uint8_t offset, const std::uint8_t count) {
        toc_controller.write(0x90u, 64u, MemoryAccessWidth::Byte);
        toc_controller.write(0x94u, 0u, MemoryAccessWidth::Byte);
        toc_controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
        std::array<std::uint8_t, 12u> request{};
        request[0] = 0x10u;
        request[2] = offset;
        request[4] = count;
        write_packet(toc_controller, request);
        static_cast<void>(toc_scheduler.advance_by(1'000u, 1u));
        std::vector<std::uint8_t> result;
        if (toc_controller.status().pio_bytes_available != 0u) {
            static_cast<void>(toc_controller.read(0x9Cu, MemoryAccessWidth::Byte));
            for (std::size_t index = 0u; index < count; index += 2u)
                append_pio_word(toc_controller, result, std::min<std::size_t>(2u, count - index));
            static_cast<void>(toc_controller.read(0x9Cu, MemoryAccessWidth::Byte));
        }
        return result;
    };
    const auto req_stat_completions = toc_completions;
    const auto req_stat = read_req_stat(0u, 10u);
    require(req_stat == std::vector<std::uint8_t>({1u, 0x80u, 0x01u, 1u, 1u,
                                                   0u, 0u, 150u, 0u, 0u}) &&
                toc_completions == req_stat_completions + 2u,
            "REQ_STAT liefert keinen zehn Byte grossen GD-ROM-/Track-/FAD-Status.");
    require(read_req_stat(2u, 4u) == std::vector<std::uint8_t>({0x01u, 1u, 1u, 0u}),
            "REQ_STAT respektiert Offset und Allocation-Length nicht.");
    const auto invalid_req_stat = read_req_stat(9u, 2u);
    require(invalid_req_stat.empty() && (toc_controller.status().ata_status & 1u) != 0u,
            "REQ_STAT akzeptiert einen Statusbereich hinter Byte zehn.");
    const auto req_stat_sense = read_taskfile_sense(toc_controller, toc_scheduler);
    require(req_stat_sense[2] == 5u && req_stat_sense[8] == 0x24u,
            "REQ_STAT-Boundsfehler liefert nicht INVALID FIELD.");

    for (const auto unsupported : std::array<std::uint8_t, 3u>{0x15u, 0x31u, 0x40u}) {
        toc_controller.write(0x9Cu, 0xA0u, MemoryAccessWidth::Byte);
        std::array<std::uint8_t, 12u> request{};
        request[0] = unsupported;
        write_packet(toc_controller, request);
        static_cast<void>(toc_scheduler.advance_by(1'000u, 1u));
        require((toc_controller.status().ata_status & 1u) != 0u,
                "Unimplementiertes benachbartes SPI-Kommando meldet Scheinerfolg.");
        const auto unsupported_sense = read_taskfile_sense(toc_controller, toc_scheduler);
        require(unsupported_sense[2] == 5u && unsupported_sense[8] == 0x20u,
                "Unimplementiertes benachbartes SPI-Kommando ist nicht INVALID COMMAND.");
    }
    return EXIT_SUCCESS;
}
