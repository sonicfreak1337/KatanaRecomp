#include "katana/platform/dreamcast.hpp"
#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/system_asic.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {
void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    using namespace katana;
    io::ExecutableImage image("synthetic-retail-boot");
    io::ImageSegment segment;
    segment.name = ".boot";
    segment.virtual_address = 0x8C010000u;
    segment.memory_size = 4u;
    segment.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u};
    segment.permissions = {true, false, true};
    image.add_segment(std::move(segment));
    image.add_entry_point(0x8C010000u);

    runtime::CpuState cpu;
    cpu.memory = runtime::Memory(0u);
    platform::DreamcastBootConfig boot_config;
    boot_config.firmware_mode = platform::FirmwareMode::HleBiosAbi;
    const auto boot = platform::boot_homebrew(cpu, image, boot_config);
    require(boot.log.front() == "firmware=hle-bios-abi" && cpu.pc == 0x8C010000u,
            "Synthetischer Retail-HLE-Bootzustand ist nicht sichtbar oder deterministisch.");

    runtime::EventScheduler scheduler;
    auto rtc_clock = std::make_shared<runtime::Sh4RtcClockDomain>(256u);
    runtime::Sh4Tmu tmu(scheduler, {1u, rtc_clock});
    runtime::Sh4Rtc rtc(scheduler, rtc_clock);
    runtime::Sh4Dmac dmac(scheduler, cpu.memory, {1u});
    runtime::InterruptController controller;
    runtime::PlatformInterruptRouter router(controller, tmu, rtc, dmac);
    const auto asic = runtime::map_dreamcast_system_asic(cpu.memory, router);
    runtime::RuntimeBlockTable blocks;
    runtime::FirmwareHandoffMap handoff;
    handoff.map_segment(
        {"main-ram", runtime::FirmwareSegmentKind::Ram, 0x8C000000u, 0x0C000000u, 0x01000000u});
    runtime::install_hle_bios_abi(cpu.memory, blocks, handoff);

    cpu.pc = runtime::hle_bios_abi_vectors()[0].handler_address;
    cpu.r[7] = 0u;
    cpu.pr = 0x8C010000u;
    runtime::BlockExecutionContext context;
    const auto* bios = blocks.lookup(cpu.pc, {});
    const auto bios_exit = bios->function(cpu, context);
    require(bios_exit.kind == runtime::BlockEndKind::Return && cpu.pc == 0x8C010000u,
            "HLE-BIOS-ABI erreicht den gemeinsamen Runtimeblock-Handoff nicht.");

    cpu.memory.write_u32(0xA05F6930u, (1u << 3u) | (1u << 12u) | (1u << 14u) | (1u << 15u));
    static_cast<void>(asic->schedule(scheduler, runtime::SystemAsicEvent::PvrVblank, 20u));
    static_cast<void>(asic->schedule(scheduler, runtime::SystemAsicEvent::MapleDma, 20u));
    static_cast<void>(asic->schedule(scheduler, runtime::SystemAsicEvent::GdromDma, 21u));
    static_cast<void>(asic->schedule(scheduler, runtime::SystemAsicEvent::AicaDma, 22u));
    require(scheduler.advance_to(22u, 4u).processed_events == 4u,
            "Retail-Systemdienste erreichen den ASIC nicht im Gastzeitbudget.");
    cpu.vbr = 0x8C000000u;
    cpu.set_interrupt_mask(0u);
    require(router.accept(cpu) && cpu.intevt == 0x000003A0u && asic->events().size() == 4u &&
                handoff.runtime_symbols().size() == 12u,
            "Gemeinsamer Retail-Bootpfad erreicht BIOS-Handoff und ASIC-IRL9 nicht.");

    std::cout << "KR_V046_RETAIL_BOOT_SERVICES_READY\n";
}
