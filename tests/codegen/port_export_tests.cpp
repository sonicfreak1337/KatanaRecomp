#include "katana/codegen/port_export.hpp"
#include "katana/ir/lower.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/platform_services.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t raw_sector_size = 2352u;
constexpr std::size_t payload_size = 2048u;
constexpr std::uint32_t data_lba = 45'000u;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception = std::exception, typename Function>
void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

struct Fixture final {
    std::filesystem::path root = std::filesystem::current_path() / "katana-port-export-fixture";

    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "disc");
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void both32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        bytes[offset + 4u + index] = static_cast<std::uint8_t>(value >> ((3u - index) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& bytes,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    bytes[offset] = length;
    both32(bytes, offset + 2u, lba);
    both32(bytes, offset + 10u, size);
    bytes[offset + 25u] = directory ? 2u : 0u;
    bytes[offset + 28u] = 1u;
    bytes[offset + 31u] = 1u;
    bytes[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

std::size_t payload_offset(const std::size_t sector, const std::size_t byte = 0u) {
    return sector * raw_sector_size + 16u + byte;
}

std::vector<std::uint8_t> boot_track(const bool immediate_trap = false) {
    std::vector<std::uint8_t> bytes(22u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 22u; ++sector) {
        bytes[sector * raw_sector_size + 15u] = 1u;
    }
    const std::string hardware = "SEGA SEGAKATANA ";
    const std::string boot_file = "BOOT.BIN        ";
    std::copy(hardware.begin(),
              hardware.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u)));
    std::copy(boot_file.begin(),
              boot_file.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x60u)));
    constexpr std::array<std::uint8_t, 12u> system_bootstrap = {
        0x01u, 0xD0u, // mov.l @(1,pc),r0 -> P2 literal at 0xAC008308
        0x2Bu, 0x40u, // jmp @r0
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // aligned padding
        0x00u, 0x00u, 0x01u, 0x8Cu // 0x8C010000
    };
    std::copy(system_bootstrap.begin(),
              system_bootstrap.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x300u)));

    const auto pvd = payload_offset(16u);
    bytes[pvd] = 1u;
    std::copy_n("CD001", 5u, bytes.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    bytes[pvd + 6u] = 1u;
    record(bytes, pvd + 156u, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    record(bytes, directory, data_lba + 21u, 24u, "BOOT.BIN;1", false);
    constexpr std::array<std::uint8_t, 24u> normal_program = {
        0x22u, 0x4Fu, // sts.l pr,@-r15: preserve the program-root sentinel
        0x0Au, 0xE0u, // mov #10,r0
        0x03u, 0x00u, // bsrf r0 -> 0x8C010012
        0x07u, 0xE2u, // delay slot: mov #7,r2
        0x26u, 0x4Fu, // lds.l @r15+,pr: restore the program-root sentinel
        0x0Bu, 0x00u, // caller rts
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // padding nop
        0x09u, 0x00u, // padding nop
        0x05u, 0xE1u, // callee: mov #5,r1
        0x0Bu, 0x00u, // callee rts
        0x09u, 0x00u  // delay-slot nop
    };
    constexpr std::array<std::uint8_t, 24u> trap_program = {
        0x00u, 0xC3u, // trapa #0
        0x0Bu, 0x00u, // unreachable rts
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
    const auto& program = immediate_trap ? trap_program : normal_program;
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

void write_binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void write_fixture(const std::filesystem::path& directory, const bool immediate_trap = false) {
    std::vector<std::uint8_t> low_track(24u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 24u; ++sector)
        low_track[sector * raw_sector_size + 15u] = 1u;
    write_binary(directory / "low.bin", low_track);
    write_binary(directory / "audio.raw", std::vector<std::uint8_t>(raw_sector_size));
    write_binary(directory / "high.bin", boot_track(immediate_trap));
    std::ofstream descriptor(directory / "disc.gdi", std::ios::trunc);
    descriptor << "3\n"
               << "1 0 4 2352 low.bin 0\n"
               << "2 30 0 2352 audio.raw 0\n"
               << "3 " << data_lba << " 4 2352 high.bin 0\n";
}

std::map<std::string, std::string> snapshot(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        result.emplace(entry.path().lexically_relative(root).generic_string(), content.str());
    }
    return result;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int run_test(const int argc, char* argv[]) {
    if (argc == 3 && (std::string(argv[1]) == "--write-fixture" ||
                      std::string(argv[1]) == "--write-trap-fixture")) {
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        write_fixture(directory, std::string(argv[1]) == "--write-trap-fixture");
        return EXIT_SUCCESS;
    }
    require(argc == 1, "Unerwartete Argumente fuer den Portexporttest.");
    using namespace katana::codegen;
    Fixture fixture;
    const auto previous_port = fixture.root / "previous-port";
    const auto published_port = fixture.root / "published-port";
    std::filesystem::create_directories(previous_port / "user-data" / "content");
    std::filesystem::create_directories(published_port / "user-data" / "content");
    {
        std::ofstream local_pack(previous_port / "user-data" / "content" /
                                 "game.katana-disc",
                                 std::ios::binary);
        local_pack << "local-retail-cache";
    }
    preserve_local_port_user_data(previous_port, published_port);
    require(!std::filesystem::exists(previous_port / "user-data") &&
                read_text(published_port / "user-data" / "content" /
                          "game.katana-disc") == "local-retail-cache",
            "Atomarer Portaustausch verliert oder kopiert lokale Disc-/Speicherdaten.");
    write_fixture(fixture.root / "disc");
    const auto gdi = fixture.root / "disc" / "disc.gdi";
    const auto runtime_boot = katana::runtime::load_dreamcast_runtime_boot(gdi);
    katana::runtime::CpuState runtime_cpu;
    const auto runtime_state =
        katana::runtime::initialize_dreamcast_runtime(runtime_cpu, runtime_boot);
    require(runtime_state.loaded_boot_bytes == 24u && runtime_cpu.pc == 0x8C010000u &&
                runtime_cpu.r[15] == 0x8D000000u &&
                runtime_cpu.vbr == katana::runtime::dreamcast_direct_boot_vector_base &&
                runtime_cpu.read_sr() == katana::runtime::dreamcast_disc_boot_status &&
                runtime_cpu.read_fpscr() == katana::runtime::dreamcast_disc_boot_fpscr &&
                runtime_cpu.gbr == katana::runtime::dreamcast_bios_handoff_gbr &&
                runtime_cpu.ssr == katana::runtime::dreamcast_bios_handoff_ssr &&
                runtime_cpu.spc == katana::runtime::dreamcast_bios_handoff_spc &&
                runtime_cpu.sgr == katana::runtime::dreamcast_direct_boot_stack &&
                runtime_cpu.dbr == katana::runtime::dreamcast_bios_handoff_dbr &&
                runtime_cpu.pr == katana::runtime::dreamcast_bios_handoff_pr && runtime_cpu.t &&
                runtime_cpu.privileged_mode() && runtime_cpu.interrupt_mask() == 15u &&
                runtime_cpu.memory.read_u16(0x8C010000u) == 0x4F22u &&
                runtime_state.runtime_blocks && runtime_state.runtime_blocks->size() == 0u &&
                runtime_state.system_asic && runtime_state.interrupt_router &&
                runtime_state.cache_control && runtime_state.io_ports &&
                runtime_state.dmac->operation() ==
                    katana::runtime::dreamcast_bios_handoff_dmaor &&
                runtime_state.holly_dma.g1->read(0x04u) == 0u &&
                runtime_state.holly_dma.g1->read(0x08u) == 0u &&
                runtime_state.holly_dma.g1->read(0xF4u) == 0x0C010800u &&
                runtime_state.holly_dma.g1->read(0xF8u) == 0u &&
                runtime_state.aica_registers->read(0x289Cu,
                                                   katana::runtime::MemoryAccessWidth::Halfword) ==
                    0x48u &&
                runtime_state.aica_registers->read(0x28A8u,
                                                   katana::runtime::MemoryAccessWidth::Byte) ==
                    0x18u &&
                runtime_state.io_ports->control_a() ==
                    katana::runtime::dreamcast_bios_handoff_pctra &&
                runtime_cpu.memory.read_u16(katana::runtime::sh4_port_data_a_address) ==
                    katana::runtime::dreamcast_composite_port_a_input &&
                runtime_state.pvr_registers->read(katana::runtime::pvr_register::SpgLoad) ==
                    0x020C0359u &&
                runtime_cpu.memory.read_u32(katana::runtime::sh4_cache_control_address) == 0u,
            "Eigenstaendiger GDI-Boot initialisiert Bootimage, privilegierten CPU-Handoff oder "
            "Speicher nicht.");
    auto pal_runtime_boot = runtime_boot;
    pal_runtime_boot.area_symbols = "E";
    katana::runtime::CpuState pal_runtime_cpu;
    const auto pal_runtime_state =
        katana::runtime::initialize_dreamcast_runtime(
            pal_runtime_cpu,
            pal_runtime_boot,
            katana::runtime::DreamcastRuntimeFirmwareMode::Direct,
            {},
            katana::runtime::DreamcastConsoleProfile::EuropePal);
    require(pal_runtime_state.io_ports->data_a() ==
                katana::runtime::dreamcast_composite_port_a_input,
            "PAL-BIOS-Handoff reicht das Latch im alternativen Pinmodus als GPIO-Ausgang durch.");
    pal_runtime_state.io_ports->write_control_a(0x00000010u);
    require((pal_runtime_state.io_ports->data_a() &
             katana::runtime::dreamcast_bios_handoff_pal_pdtra) != 0u,
            "PAL-BIOS-Handoff setzt den Broadcast-Portzustand nicht.");
    runtime_cpu.memory.write_u32(katana::runtime::sh4_cache_control_address,
                                 katana::runtime::Sh4CacheControl::instruction_invalidate);
    require(runtime_state.cache_control->instruction_invalidation_count() == 1u &&
                runtime_cpu.memory.read_u32(katana::runtime::sh4_cache_control_address) == 0u,
            "Produktiver GDI-Boot bindet das SH-4-Cache-Control-Register nicht ein.");
    static_cast<void>(runtime_state.code_tracker->register_block(
        {"sq-code",
         0x0C000000u,
         32u,
         "synthetic",
         {},
         katana::runtime::ExecutableBlockOrigin::ImageSegment}));
    runtime_cpu.memory.write_u32(0xFF000038u, 0x0Cu);
    for (std::uint32_t offset = 0u; offset < 32u; offset += 4u)
        runtime_cpu.memory.write_u32(0xE0000000u + offset, 0x03020100u + offset * 0x01010101u);
    require(runtime_state.store_queues->prefetch(0xE0000000u) &&
                runtime_cpu.memory.read_u32(0x0C000000u) == 0x03020100u &&
                runtime_state.code_tracker->invalidation_count() == 1u,
            "Produktive Store Queue uebertraegt keine 32 Byte nach RAM oder invalidiert Code.");
    runtime_cpu.memory.write_u32(0xFF00003Cu, 0x10u);
    runtime_cpu.memory.write_u32(0xE2000020u, 0x80000000u);
    const auto ta_position_before_store_queue = runtime_state.pvr_registers->read(
        katana::runtime::pvr_register::TaIspCurrent);
    require(runtime_state.store_queues->prefetch(0xE2000020u) &&
                runtime_state.store_queue_transfers->back().target ==
                    katana::runtime::StoreQueueTarget::TileAccelerator &&
                runtime_state.store_queue_transfers->back().bytes[0] == 0u &&
                runtime_state.pvr_registers->read(
                    katana::runtime::pvr_register::TaIspCurrent) ==
                    ta_position_before_store_queue + 32u,
            "Produktive Store Queue verliert QACR-basierten TA-Transfer oder TA-Zeiger.");

    const auto gdrom_events_before_packet = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::GdromCommand;
        });
    runtime_state.gdrom->write(0x9Cu, 0xA0u, katana::runtime::MemoryAccessWidth::Byte);
    for (std::size_t word = 0u; word < 6u; ++word)
        runtime_state.gdrom->write(0x80u, 0u, katana::runtime::MemoryAccessWidth::Halfword);
    const auto packet_completion = runtime_state.scheduler->advance_by(1'000u, 1u);
    const auto gdrom_events_after_completion = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::GdromCommand;
        });
    require(packet_completion.processed_events == 1u &&
                gdrom_events_after_completion == gdrom_events_before_packet + 1u,
            "GD-ROM-Completion setzt den Command-IRQ nicht atomar im Completion-Ereignis.");
    static_cast<void>(runtime_state.gdrom->read(
        0x9Cu, katana::runtime::MemoryAccessWidth::Byte));
    const auto post_ack = runtime_state.scheduler->advance_to(
        runtime_state.scheduler->current_cycle(), 1u);
    const auto gdrom_events_after_ack = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::GdromCommand;
        });
    require(post_ack.processed_events == 0u &&
                gdrom_events_after_ack == gdrom_events_after_completion,
            "STATUS-Quittierung laesst einen verspaeteten Same-Cycle-GD-ROM-IRQ zurueck.");
    const auto ta_packets_before_channel2 = runtime_state.pvr_ta_fifo->metrics().packets;
    const auto pvr_dma_events_before_channel2 = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::Channel2Dma;
        });
    for (std::uint32_t offset = 0u; offset < 32u; offset += 4u)
        runtime_cpu.memory.write_u32(0x8C000800u + offset, 0u);
    runtime_state.dmac->write_source(2u, 0x8C000800u);
    runtime_state.dmac->write_count(2u, 1u);
    runtime_state.dmac->write_control(2u, 0x00001841u);
    runtime_state.dmac->write_operation(katana::runtime::Sh4Dmac::master_enable);
    runtime_state.system_bus_control->write(
        katana::runtime::system_bus_register::Channel2Destination, 0x10000000u);
    runtime_state.system_bus_control->write(
        katana::runtime::system_bus_register::Channel2Length, 32u);
    runtime_state.system_bus_control->write(
        katana::runtime::system_bus_register::Channel2Start, 1u);
    static_cast<void>(runtime_state.scheduler->advance_by(32u, 1u));
    const auto pvr_dma_events_after_channel2 = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::Channel2Dma;
        });
    require(runtime_state.pvr_ta_fifo->metrics().packets ==
                    ta_packets_before_channel2 + 1u &&
                runtime_state.dmac->completed_transfer_units(2u) == 1u &&
                (runtime_state.dmac->control(2u) & katana::runtime::Sh4Dmac::transfer_end) != 0u &&
                runtime_state.system_bus_control->read(
                    katana::runtime::system_bus_register::Channel2Start) == 0u &&
                runtime_state.system_bus_control->read(
                    katana::runtime::system_bus_register::Channel2Length) == 0u &&
                pvr_dma_events_after_channel2 == pvr_dma_events_before_channel2 + 1u,
            "Produktiver Channel-2-DMAC erreicht TA-FIFO, Abschlussstatus oder System-ASIC nicht.");
    katana::runtime::CpuState hle_runtime_cpu;
    const auto hle_runtime_state = katana::runtime::initialize_dreamcast_runtime(
        hle_runtime_cpu, runtime_boot, katana::runtime::DreamcastRuntimeFirmwareMode::HleBiosAbi);
    require(hle_runtime_cpu.memory.read_u32(0x8C0000B0u) ==
                    katana::runtime::hle_bios_abi_vectors()[0].handler_address &&
                hle_runtime_cpu.pc ==
                    katana::runtime::dreamcast_system_bootstrap_entry_address &&
                hle_runtime_state.loaded_system_bootstrap_bytes ==
                    katana::runtime::dreamcast_system_bootstrap_size &&
                hle_runtime_cpu.memory.read_u16(
                    katana::runtime::dreamcast_system_bootstrap_entry_address) == 0xD001u &&
                hle_runtime_state.runtime_blocks->size() == 7u &&
                hle_runtime_cpu.memory.read_u32(0x8C002400u) == 0xFFFFFFFFu &&
                hle_runtime_state.runtime_blocks
                    ->lookup(katana::runtime::hle_bios_gdrom2_direct_alias_address, {})
                    .has_value(),
            "Produktiver GDI-HLE-Runtimepfad installiert BIOS-ABI oder Disc-Bootstrap nicht.");
    hle_runtime_cpu.memory.write_u32(0x8C002400u, 0xC001D00Du);
    hle_runtime_cpu.memory.write_u32(
        katana::runtime::dreamcast_system_bootstrap_entry_address, 0xDEADBEEFu);
    hle_runtime_cpu.memory.write_u32(
        katana::runtime::dreamcast_disc_boot_address, 0xDEADBEEFu);
    hle_runtime_state.dmac->write_operation(katana::runtime::Sh4Dmac::master_enable);
    hle_runtime_state.aica_registers->write(
        0x289Cu, 0u, katana::runtime::MemoryAccessWidth::Halfword);
    hle_runtime_state.cache_control->write(
        katana::runtime::Sh4CacheControl::operand_ram_enable);
    hle_runtime_cpu.memory.write_u32(katana::runtime::sh4_on_chip_ram_address, 0x12345678u);
    require(hle_runtime_state.code_tracker->page_generation(
                katana::runtime::sh4_on_chip_ram_address) == 0u,
            "Reine OCRAM-Stackdaten erzeugen unnoetige Codeinvalidierungsprovenienz.");
    hle_runtime_state.io_ports->write_control_a(0x10u);
    hle_runtime_state.io_ports->write_data_a(0u);
    const auto system_vector = katana::runtime::hle_bios_abi_vectors()[5];
    const auto system_handle = hle_runtime_state.runtime_blocks->lookup(
        system_vector.handler_address, {});
    const auto system_block = system_handle
                                  ? hle_runtime_state.runtime_blocks->resolve(*system_handle)
                                  : std::nullopt;
    require(system_block.has_value(), "SYSTEM-1-Runtimeblock fehlt im produktiven HLE-Pfad.");
    hle_runtime_cpu.pc = system_vector.handler_address;
    hle_runtime_cpu.r[4] = 1u;
    katana::runtime::BlockExecutionContext lifecycle_context;
    lifecycle_context.scheduler_cycle = 77u;
    try {
        static_cast<void>(system_block->get().function(hle_runtime_cpu, lifecycle_context));
        require(false, "SYSTEM 1 kehrt im produktiven HLE-Pfad zurueck.");
    } catch (const katana::runtime::PlatformLifecycleExit& exit) {
        require(exit.reason() == katana::runtime::PlatformLifecycleExitReason::BiosMenu &&
                    exit.evidence().guest_cycle == 77u &&
                    hle_runtime_cpu.pc == system_vector.handler_address &&
                    hle_runtime_cpu.memory.read_u32(
                        katana::runtime::dreamcast_system_bootstrap_entry_address) ==
                        0xDEADBEEFu &&
                    hle_runtime_cpu.memory.read_u32(
                        katana::runtime::dreamcast_disc_boot_address) == 0xDEADBEEFu &&
                    hle_runtime_cpu.memory.read_u32(0x8C002400u) == 0xC001D00Du &&
                    hle_runtime_state.dmac->operation() ==
                        katana::runtime::Sh4Dmac::master_enable &&
                    hle_runtime_state.aica_registers->read(
                        0x289Cu, katana::runtime::MemoryAccessWidth::Halfword) == 0u &&
                    hle_runtime_cpu.memory.read_u32(
                        katana::runtime::sh4_cache_control_address) ==
                        katana::runtime::Sh4CacheControl::operand_ram_enable &&
                    hle_runtime_state.io_ports->control_a() == 0x10u,
                "SYSTEM 1 mutiert Bootbytes oder Geraetezustand statt als BIOS-Menue zu enden.");
    }
    const auto render_done_count = [&] {
        return std::count_if(hle_runtime_state.system_asic->events().begin(),
                             hle_runtime_state.system_asic->events().end(),
                             [](const auto& event) {
                                 return event.event ==
                                        katana::runtime::SystemAsicEvent::PvrRenderDone;
                             });
    };
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::FramebufferWriteControl, 7u);
    const auto render_done_before_failure = render_done_count();
    const auto render_completions_before_failure =
        hle_runtime_state.pvr_registers->render_completion_count();
    hle_runtime_state.pvr_registers->write(katana::runtime::pvr_register::StartRender, 1u);
    static_cast<void>(hle_runtime_state.scheduler->advance_by(2'000u, 64u));
    require(hle_runtime_state.pvr_registers->render_completion_count() ==
                    render_completions_before_failure + 1u &&
                render_done_count() == render_done_before_failure &&
                hle_runtime_state.pvr_renderer->first_error().has_value(),
            "Fehlgeschlagener PVR-Renderpfad signalisiert RenderDone.");
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::FramebufferWriteControl, 0u);
    constexpr std::uint32_t render_background = 0x00100000u;
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::ParameterBase, render_background);
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::BackgroundPlaneConfig, 1u << 24u);
    hle_runtime_state.vram->write_u32(render_background, 0u);
    hle_runtime_state.vram->write_u32(
        render_background + 4u, (1u << 29u) | (2u << 22u) | (1u << 20u));
    hle_runtime_state.vram->write_u32(render_background + 8u, 0u);
    for (const auto vertex : {render_background + 12u,
                              render_background + 28u,
                              render_background + 44u}) {
        hle_runtime_state.vram->write_u32(vertex, 0u);
        hle_runtime_state.vram->write_u32(vertex + 4u, 0u);
        hle_runtime_state.vram->write_u32(vertex + 8u, 0u);
        hle_runtime_state.vram->write_u32(vertex + 12u, 0xFF204060u);
    }
    const auto render_completions_before_success =
        hle_runtime_state.pvr_registers->render_completion_count();
    hle_runtime_state.pvr_registers->write(katana::runtime::pvr_register::StartRender, 1u);
    static_cast<void>(hle_runtime_state.scheduler->advance_by(2'000u, 64u));
    require(hle_runtime_state.pvr_registers->render_completion_count() ==
                    render_completions_before_success + 1u &&
                render_done_count() == render_done_before_failure + 1u,
            "Gueltiger PVR-Renderabschluss erreicht RenderDone nicht deterministisch.");
    auto input = std::make_shared<katana::runtime::ReplayInputBackend>(
        std::vector<katana::runtime::ControllerState>{{}});
    hle_runtime_state.maple->attach(
        0u, 0u, std::make_shared<katana::runtime::MapleControllerDevice>(input));
    static_cast<void>(hle_runtime_state.maple->exchange(
        0u, 0u, {katana::runtime::MapleCommand::GetCondition, {}}));
    hle_runtime_cpu.memory.write_u32(0x8C000400u, 150u);
    hle_runtime_cpu.memory.write_u32(0x8C000404u, 1u);
    hle_runtime_cpu.memory.write_u32(0x8C000408u, 0x8C001000u);
    hle_runtime_cpu.r[4] = 16u;
    hle_runtime_cpu.r[5] = 0x8C000400u;
    static_cast<void>(hle_runtime_state.gdrom->bios_call(hle_runtime_cpu, 0u, 0u));
    static_cast<void>(hle_runtime_state.gdrom->bios_call(hle_runtime_cpu, 2u, 0u));
    static_cast<void>(hle_runtime_state.scheduler->advance_by(2'000u, 8u));
    hle_runtime_state.aica->interrupts().set_enabled(1u);
    hle_runtime_state.aica->interrupts().request(1u);
    const auto has_asic_event = [&](const katana::runtime::SystemAsicEvent expected) {
        return std::any_of(hle_runtime_state.system_asic->events().begin(),
                           hle_runtime_state.system_asic->events().end(),
                           [&](const auto& event) { return event.event == expected; });
    };
    require(has_asic_event(katana::runtime::SystemAsicEvent::PvrRenderDone),
            "Produktives PVR-RenderDone erreicht das System-ASIC nicht.");
    require(has_asic_event(katana::runtime::SystemAsicEvent::MapleDma),
            "Produktives Maple-DMA erreicht das System-ASIC nicht.");
    require(has_asic_event(katana::runtime::SystemAsicEvent::GdromCommand),
            "Produktive GD-ROM-Completion erreicht das System-ASIC nicht.");
    require(has_asic_event(katana::runtime::SystemAsicEvent::AicaInterrupt),
            "Produktiver AICA-Interrupt erreicht das System-ASIC nicht.");
    const auto output = fixture.root / "port";
    const PortExportOptions options{"synthetic_game", "0.37.0-dev", {1u, 4096u}};

    const auto first = export_dreamcast_port_project(gdi, output, options);
    const auto generated_before = snapshot(output / "generated");
    const auto unit =
        std::find_if(generated_before.begin(), generated_before.end(), [](const auto& entry) {
            return entry.first.starts_with("code/unit-00000-") && entry.first.ends_with(".cpp");
        });
    require(first.functions == 3u && first.partitions == 3u && first.checkpoints.size() == 8u &&
                first.checkpoints.back() == "port-project-written",
            "Synthetische GDI durchlaeuft den Portexport nicht vollstaendig.");
    require(std::filesystem::exists(output / "content" / "game.katana-install") &&
                !std::filesystem::exists(output / "content" / "game.katana-disc") &&
                read_text(output / ".gitignore").find("*.katana-disc") != std::string::npos &&
                read_text(output / "INSTALL_ORIGINAL_DISC.txt").find("ORIGINAL DISC REQUIRED") !=
                    std::string::npos &&
                read_text(output / "INSTALL_ORIGINAL_DISC.txt").find("never modified or deleted") !=
                    std::string::npos &&
                first.disc_tracks == 3u,
            "Portexport trennt distributionsfaehige Recipe und lokalen Retailcache nicht.");
    const auto recipe = katana::runtime::parse_disc_install_recipe(first.disc_install_recipe);
    require(recipe.tracks.size() == 3u && recipe.content_identity == first.content_identity &&
                recipe.job_generation == first.job_generation &&
                read_text(first.disc_install_recipe).find("low.bin") == std::string::npos &&
                read_text(first.disc_install_recipe).find(fixture.root.string()) ==
                    std::string::npos,
            "Generische Disc-Recipe verliert Bindung oder enthaelt private Quellpfade.");
    require(unit != generated_before.end(),
            "Portexport besitzt keine deterministische Translation Unit.");
    std::size_t entry_metadata_count = 0u;
    bool p2_pc_relative_literal = false;
    for (const auto& [path, content] : generated_before) {
        if (path.starts_with("code/unit-") && path.ends_with(".cpp")) {
            require(content.find("generated_entry_address = 0xAC008300u") != std::string::npos,
                    "Portpartition besitzt einen abweichenden globalen Programmeinstieg.");
            p2_pc_relative_literal =
                p2_pc_relative_literal || content.find("0xAC008308u") != std::string::npos;
            ++entry_metadata_count;
        }
    }
    require(entry_metadata_count == 3u && p2_pc_relative_literal,
            "Mehrteiliger Portexport verliert P2-Einstieg oder PC-relativen P2-Literalzugriff.");
    for (const auto& path : {"include/katana_port.hpp",
                             "code/runtime-dispatch.cpp",
                             "metadata/port-project.json",
                             "metadata/provenance.json",
                             "metadata/source-map.json",
                             "metadata/cfg.json",
                             "metadata/cfg.dot",
                             "metadata/callgraph.json",
                             "metadata/callgraph.dot",
                             "katana-port.cmake"}) {
        require(generated_before.contains(path),
                "Portexport verliert Artefakt: " + std::string(path));
    }
    require(
        generated_before.at("katana-port.cmake").find("add_executable(synthetic_game") !=
                std::string::npos &&
            generated_before.at("katana-port.cmake").find("katana_runtime") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("dispatch_indirect") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("execute_dynamic_sh4_block(cpu, *active_services, 1u)") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("count < 64u") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("candidate.instructions = 1u") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("generated-block-AC008300") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("register_executable_block(table, services, 0xAC008300u") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"execution_coverage_contract\":\"validated-demand-v1\"") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"dispatch_paths_without_validation\":0") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("generated-block-8C010000") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("register_executable_block(table, services, 0x8C010000u") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("services.allow_executable_block_chaining(") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("executed_dispatch_blocks >= block_budget") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("KATANA_PORT_BLOCK_PROGRESS") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("append_static_block(static_blocks, 0x8C010000u") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("static_blocks.push_back({") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("if (const auto registered_handle = table.lookup(0x8C010000u") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("8u, katana::runtime::BlockEndKind::Call") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("SLEEP besitzt kein Wakeup-Ereignis") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("Schedulerbudget erschoepft") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("Gastzyklusbudget erschoepft") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("Runtime-Blockbudget erschoepft") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("KATANA_PORT_BLOCK_LIMIT") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("DispatchChainBoundary::ProgramRoot") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("cpu.pc == program_return_sentinel") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("target = cpu.pc;\n            kind = "
                          "katana::runtime::IndirectDispatchKind::TailJump") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("DispatchChainBoundary::NestedCall") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("poll_host_lifecycle") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("PlatformShutdownRequested") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("blocks < 1000000u") ==
                std::string::npos &&
            generated_before.at("include/katana_port.hpp").find("runtime_only_profile_json") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("runtime_only_dispatch_share_ppm") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("KATANA_RUNTIME_DISPATCH_DIAGNOSTICS") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("KATANA_PORT_DIAGNOSTICS_FULL") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("KATANA_RUNTIME_DISPATCH_EVENTS") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("serialize_json(true)") !=
                std::string::npos &&
            std::filesystem::exists(output / "CMakeLists.txt") &&
            read_text(output / "CMakeLists.txt").find("katana_core") == std::string::npos &&
            std::filesystem::exists(output / "src" / "main.cpp") &&
            read_text(output / "src" / "main.cpp")
                    .find("DreamcastRuntimeFirmwareMode::HleBiosAbi") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("DreamcastConsoleProfile::JapanNtsc") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PLATFORM_LIFECYCLE_EXIT") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_GDROM_BIOS_EVENTS") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PORT_MEMORY_PROBES") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("memory_probe_value=") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("memory.peek_u32") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu.memory.read_u32(address)") == std::string::npos &&
            read_text(output / "src" / "main.cpp").find("load_dreamcast_runtime_boot") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("source.info().content_identity != expected_content_identity") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("verify_boot_identity(boot)") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("create_native_video_output") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_host_events") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("next_lifecycle_poll_") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PORT_BLOCK_LIMIT\") == nullptr") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("can_chain_executable_block(std::uint32_t address)") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("chainable_blocks_.contains(address)") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("active_block_variant_->runtime_generation") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu_.retired_guest_instructions - chain_retired_baseline_") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("*event <= state_.scheduler->current_cycle() + pending") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("result.processed_events != 0u") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("!lifecycle_test.empty()") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_HOST_SHUTDOWN") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_guest_frame_proof") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("result.guest_frame_proven") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KATANA_PORT_PROGRESS") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("set_mmio_access_tracking") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("highest_pending") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("runtime_materialization_status") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("exception_cause=") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.expevt") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.spc") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("framebuffer.configure(640u") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_guest_frame") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("result.frame_presented") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_FIRST_GUEST_FRAME") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_FIRST_PRESENTED_FRAME") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_guest_frame_proof") <
                read_text(output / "src" / "main.cpp")
                    .find("run_runtime(cpu, services, *state.runtime_blocks)") &&
            read_text(output / "src" / "main.cpp").find("HostRuntimeSession") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("DreamcastMutableStorage::open") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("HostPacer") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("mutable_storage->save") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KATANA_HOST_PACING_ERROR") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("audio_hash") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("source-identity-mismatch") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("weakly_canonical") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("source.parent_path().string()") ==
                std::string::npos,
        "Portprojekt besitzt keinen ausfuehrbaren GDI-/Runtimevertrag.");
    const auto& runtime_dispatch = generated_before.at("code/runtime-dispatch.cpp");
    const auto retire_marker = runtime_dispatch.find(
        "const auto retired_before = cpu.retired_guest_instructions");
    const auto execute_marker = runtime_dispatch.find(
        "selected_block->get().function(cpu, *active_context)", retire_marker);
    const auto scheduler_marker =
        runtime_dispatch.find("active_services->consume_guest_cycles(", execute_marker);
    const auto interrupt_marker =
        runtime_dispatch.find("active_services->poll_interrupt().has_value()", scheduler_marker);
    require(retire_marker != std::string::npos && execute_marker != std::string::npos &&
                scheduler_marker != std::string::npos && interrupt_marker != std::string::npos &&
                retire_marker < execute_marker && execute_marker < scheduler_marker &&
                scheduler_marker < interrupt_marker,
            "Gastzyklen oder Interruptannahme liegen nicht hinter der ausgefuehrten Blocksemantik.");
    const auto require_chain_registration = [&](const std::string_view end_kind,
                                                const bool expected) {
        const auto marker = ", katana::runtime::BlockEndKind::" + std::string(end_kind);
        const auto marker_position = runtime_dispatch.find(marker);
        require(marker_position != std::string::npos,
                "Portfixture besitzt keine Endklasse " + std::string(end_kind) + ".");
        const auto address_begin = runtime_dispatch.rfind("0x", marker_position);
        const auto address_end = runtime_dispatch.find('u', address_begin);
        require(address_begin != std::string::npos && address_end != std::string::npos,
                "Portfixture verliert die Blockadresse vor " + std::string(end_kind) + ".");
        const auto address = runtime_dispatch.substr(address_begin, address_end - address_begin);
        const auto registration =
            "services.allow_executable_block_chaining(" + address + "u)";
        require((runtime_dispatch.find(registration) != std::string::npos) == expected,
                "Lokales Chaining behandelt Endklasse " + std::string(end_kind) +
                    " nicht als Hostgrenze.");
    };
    require_chain_registration("Call", true);
    require_chain_registration("Return", false);
    std::string portable_content;
    for (const auto& [path, content] : generated_before) {
        static_cast<void>(path);
        portable_content += content;
    }
    require(portable_content.find(fixture.root.string()) == std::string::npos &&
                portable_content.find("disc.gdi") == std::string::npos &&
                portable_content.find("high.bin") == std::string::npos,
            "Portartefakte enthalten absolute oder private Disc-/Trackpfade.");

    const auto guarded_disc = katana::platform::load_dreamcast_gdi_boot(gdi);
    auto guarded_image = katana::platform::make_dreamcast_disc_executable(guarded_disc);
    const auto guarded_boot_segment = std::find_if(
        guarded_image.segments().begin(), guarded_image.segments().end(), [](const auto& segment) {
            return segment.virtual_address == katana::platform::dreamcast_disc_boot_address;
        });
    require(guarded_image.segments().size() == 2u &&
                guarded_boot_segment != guarded_image.segments().end() &&
                guarded_boot_segment->kind == katana::io::SegmentKind::Mixed &&
                guarded_boot_segment->source_kind ==
                    katana::io::ImageSourceKind::DiscBootFile &&
                guarded_boot_segment->bytes.size() == guarded_boot_segment->memory_size,
            "GDI-Loader markiert die Bootdatei pauschal als Code oder erfindet Zero-Fill.");
    auto guarded_analysis = katana::analysis::analyze_control_flow(guarded_image);
    require(!guarded_analysis.indirect_control_flow.empty() &&
                !guarded_analysis.resolved_edges.empty(),
            "Guarded-Portfixture besitzt keine indirekte Kandidatenkante.");
    guarded_analysis.indirect_control_flow.front().status =
        katana::analysis::ResolutionStatus::Guarded;
    guarded_analysis.indirect_control_flow.front().evidence =
        katana::analysis::ControlFlowEvidence::GuardedComplete;
    guarded_analysis.resolved_edges.front().guarded = true;
    auto guarded_program = katana::ir::lower_program(guarded_analysis);
    std::vector<katana::io::InputProvenance> guarded_inputs;
    guarded_inputs.push_back(katana::io::capture_input_provenance("gdi-descriptor", gdi));
    for (const auto& track : guarded_disc.source->descriptor().tracks)
        guarded_inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    const auto guarded_output = fixture.root / "guarded-port";
    const auto guarded_export =
        export_dreamcast_port_project({guarded_image,
                                       guarded_analysis,
                                       guarded_program,
                                       guarded_inputs,
                                       katana::platform::dreamcast_disc_boot_address,
                                       katana::platform::dreamcast_disc_boot_address,
                                       guarded_disc.boot_file.size(),
                                       "guarded-fixture"},
                                      guarded_output,
                                      options);
    const auto guarded_sources = snapshot(guarded_output / "generated");
    std::string guarded_text;
    for (const auto& [path, content] : guarded_sources) {
        if (path.starts_with("code/unit-")) guarded_text += content;
    }
    const auto& guarded_metadata = guarded_sources.at("metadata/port-project.json");
    require(guarded_export.functions != 0u && guarded_text.find("default:") != std::string::npos &&
                guarded_text.find("unresolved_call") != std::string::npos &&
                guarded_metadata.find("\"guarded_control_flow\":1") != std::string::npos &&
                guarded_metadata.find("\"guarded_complete_control_flow\":1") != std::string::npos &&
                guarded_metadata.find("\"guarded_partial_control_flow\":0") != std::string::npos &&
                guarded_metadata.find("\"unresolved_control_flow\":0") != std::string::npos,
            "Guarded-Kandidaten erreichen Portcodegen oder dynamischen Default nicht.");

    auto runtime_only_analysis = guarded_analysis;
    auto& runtime_only_resolution = runtime_only_analysis.indirect_control_flow.front();
    runtime_only_resolution.status = katana::analysis::ResolutionStatus::Unresolved;
    runtime_only_resolution.evidence = katana::analysis::ControlFlowEvidence::RuntimeOnly;
    runtime_only_resolution.target.reset();
    runtime_only_resolution.targets.clear();
    runtime_only_resolution.analysis_candidates.clear();
    runtime_only_resolution.reason = "synthetic-runtime-contract";
    runtime_only_analysis.resolved_edges.clear();
    const auto runtime_only_program = katana::ir::lower_program(runtime_only_analysis);
    const auto runtime_only_output = fixture.root / "runtime-only-port";
    static_cast<void>(export_dreamcast_port_project({guarded_image,
                                                     runtime_only_analysis,
                                                     runtime_only_program,
                                                     guarded_inputs,
                                                     katana::platform::dreamcast_disc_boot_address,
                                                     katana::platform::dreamcast_disc_boot_address,
                                                     guarded_disc.boot_file.size(),
                                                     "runtime-only-fixture"},
                                                    runtime_only_output,
                                                    options));
    const auto runtime_only_sources = snapshot(runtime_only_output / "generated");
    std::string runtime_only_text;
    for (const auto& [path, content] : runtime_only_sources)
        if (path.starts_with("code/unit-")) runtime_only_text += content;
    require(runtime_only_text.find("runtime_only_jump") != std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":1") != std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Portexport verliert den validierenden Runtime-only-Vertrag.");

    {
        std::ofstream user(output / "src" / "notes.txt", std::ios::trunc);
        user << "keep-user-file\n";
    }
    const auto second = export_dreamcast_port_project(gdi, output, options);
    require(generated_before == snapshot(output / "generated") && second.removed_files == 0u,
            "Identische Portregenerierung ist nicht bytegleich.");
    std::ifstream user(output / "src" / "notes.txt");
    std::ostringstream user_content;
    user_content << user.rdbuf();
    require(user_content.str() == "keep-user-file\n",
            "Portregenerierung hat eine handgeschriebene Nutzerdatei veraendert.");

    auto low = std::vector<std::uint8_t>(24u * raw_sector_size);
    low.front() = 0xA5u;
    write_binary(fixture.root / "disc" / "low.bin", low);
    const auto provenance_before = generated_before.at("metadata/provenance.json");
    static_cast<void>(export_dreamcast_port_project(gdi, output, options));
    const auto changed = snapshot(output / "generated");
    require(changed.at("metadata/provenance.json") != provenance_before &&
                changed.at(unit->first) == unit->second &&
                std::filesystem::exists(output / "src" / "notes.txt"),
            "Geaenderte Eingabe invalidiert Provenienz nicht gezielt oder loescht Nutzerdateien.");

    auto invalid_options = options;
    invalid_options.target_name = "../invalid";
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(export_dreamcast_port_project(gdi, output, invalid_options)); },
        "Unportabler Port-Zielname wurde akzeptiert.");

    auto protected_options = options;
    protected_options.forbidden_source_root = fixture.root;
    require_failure<std::invalid_argument>(
        [&] {
            static_cast<void>(export_dreamcast_port_project(
                gdi, fixture.root / "generated-commercial-port", protected_options));
        },
        "Portausgabe innerhalb des geschuetzten Quellbaums wurde akzeptiert.");
    const auto link = std::filesystem::temp_directory_path() / "katana-port-parent-link";
    std::error_code link_error;
    std::filesystem::remove(link, link_error);
    link_error.clear();
    std::filesystem::create_directory_symlink(fixture.root, link, link_error);
    if (!link_error) {
        require_failure<std::invalid_argument>(
            [&] {
                static_cast<void>(export_dreamcast_port_project(
                    gdi, link / "through-parent-link", protected_options));
            },
            "Symlink-Elternpfad umgeht den geschuetzten Quellbaum.");
        std::filesystem::remove(link, link_error);
    }

    auto incomplete_track = boot_track();
    incomplete_track[payload_offset(21u, 2u)] = 0x09u;
    incomplete_track[payload_offset(21u, 3u)] = 0x00u;
    write_binary(fixture.root / "disc" / "high.bin", incomplete_track);
    const auto incomplete_output = fixture.root / "incomplete-port";
    static_cast<void>(export_dreamcast_port_project(gdi, incomplete_output, options));
    const auto inferred_runtime_sources = snapshot(incomplete_output / "generated");
    std::string inferred_runtime_text;
    for (const auto& [path, content] : inferred_runtime_sources)
        if (path.starts_with("code/unit-")) inferred_runtime_text += content;
    require(inferred_runtime_text.find("runtime_only_jump") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":2") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Allgemeiner Runtimezeiger erreicht den validierenden Portvertrag nicht.");

    std::cout << "KR-3507/KR-4502/KR-4507 reproduzierbarer Port-Projektexport erfolgreich.\n";
    return EXIT_SUCCESS;
}

int main(const int argc, char* argv[]) {
    try {
        return run_test(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
