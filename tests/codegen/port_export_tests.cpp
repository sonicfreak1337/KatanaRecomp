#include "katana/codegen/port_export.hpp"
#include "katana/ir/lower.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/platform_services.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t raw_sector_size = 2352u;
constexpr std::size_t payload_size = 2048u;
constexpr std::uint32_t data_lba = 45'000u;

std::vector<std::string> observed_progress;

void observe_progress(const std::string_view phase) {
    observed_progress.emplace_back(phase);
}

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
        0x00u, 0xA0u, // bra 0x8C010004: force a locally chainable static entry block
        0x22u, 0x4Fu, // delay slot: sts.l pr,@-r15, preserve the root sentinel
        0x08u, 0xE0u, // mov #8,r0
        0x03u, 0x00u, // bsrf r0 -> 0x8C010012 from a second, dynamic call block
        0x07u, 0xE2u, // delay slot: mov #7,r2
        0x26u, 0x4Fu, // lds.l @r15+,pr: restore the program-root sentinel
        0x0Bu, 0x00u, // caller rts
        0x09u, 0x00u, // delay-slot nop
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

std::vector<std::uint8_t> poll_loop_boot_track() {
    auto bytes = boot_track();
    constexpr std::array<std::uint8_t, 24u> program = {
        0x03u, 0xD3u, // loop: mov.l @(3,pc),r3 (conservative loop-local read)
        0x04u, 0xD0u, // mov.l @(4,pc),r0 (proven loop guard read)
        0x08u, 0x20u, // tst r0,r0
        0xFBu, 0x89u, // bt 0x8C010000
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // aligned padding
        0x09u, 0x00u, // aligned padding
        0x11u, 0x11u, 0x11u, 0x11u, // conservative source at 0x8C010010
        0x00u, 0x00u, 0x00u, 0x00u  // guard source at 0x8C010014
    };
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

std::string hex_symbol(const std::uint32_t address) {
    std::ostringstream output;
    output << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << address;
    return output.str();
}

std::size_t occurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0u;
    for (auto offset = text.find(needle); offset != std::string_view::npos;
         offset = text.find(needle, offset + needle.size()))
        ++count;
    return count;
}

void disabled_product_materializer_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockMaterializationPolicy policy;
    policy.enabled = false;
    DemandBlockMaterializer materializer(modules, blocks, &tracker, policy, {});
    IndirectDispatchMetrics metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x1000u;
    request.target = 0x2000u;
    request.source = {request.callsite, canonical_physical_address(request.callsite)};
    request.resolution_origin = DispatchResolutionOrigin::RuntimeOnly;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    DispatchDiagnosticRecorder diagnostics;
    request.diagnostics = &diagnostics;
    request.metrics = &metrics;
    request.materializer = &materializer;
    bool typed_abort = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError& error) {
        typed_abort = error.metrics_json().find("\"error\":\"unknown-target\"") !=
                      std::string::npos;
    }
    const auto profile = metrics.runtime_only_sites().find(request.callsite);
    require(typed_abort && request.callsite != request.target &&
                materializer.last_failure() == MaterializationFailure::Disabled &&
                materializer.metrics().requests == 1u && materializer.metrics().misses == 1u &&
                materializer.metrics().first_failure == MaterializationFailure::Disabled &&
                materializer.metrics().first_failure_target == request.target &&
                metrics.misses() == 1u && metrics.runtime_only_misses() == 1u &&
                metrics.runtime_only_site_count() == 1u &&
                profile != metrics.runtime_only_sites().end() && profile->second.calls == 1u &&
                profile->second.misses == 1u && profile->second.targets.size() == 1u &&
                profile->second.targets.front() == request.target &&
                !metrics.runtime_only_sites().contains(request.target) &&
                diagnostics.events().size() == 1u &&
                diagnostics.events().front().callsite == request.callsite &&
                diagnostics.events().front().source_virtual == request.callsite &&
                diagnostics.events().front().virtual_target == request.target &&
                diagnostics.events().front().origin == DispatchResolutionOrigin::RuntimeOnly &&
                blocks.size() == 0u,
            "Produktmaterializer setzt ungebundenen Code nicht typisiert und ohne Ausfuehrung ab.");
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
    disabled_product_materializer_regression();
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
                runtime_cpu.memory.read_u16(0x8C010000u) == 0xA000u &&
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
    auto pal_runtime_cpu_storage = std::make_unique<katana::runtime::CpuState>();
    auto& pal_runtime_cpu = *pal_runtime_cpu_storage;
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
    runtime_state.dmac->write_control(2u, 0x000012C1u);
    runtime_state.dmac->write_operation(0x00008201u);
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
    const PortExportOptions options{"synthetic_game",
                                    "0.37.0-dev",
                                    {1u, 4096u},
                                    {},
                                    false,
                                    "japan-ntsc",
                                    observe_progress};

    observed_progress.clear();
    const auto first = export_dreamcast_port_project(gdi, output, options);
    const auto generated_before = snapshot(output / "generated");
    const auto generated_main = read_text(output / "src" / "main.cpp");
    const auto& runtime_dispatch = generated_before.at("code/runtime-dispatch.cpp");
    std::string runtime_dispatch_shards;
    std::size_t runtime_dispatch_shard_count = 0u;
    for (const auto& [path, content] : generated_before) {
        if (!path.starts_with("code/runtime-dispatch-shard-") || !path.ends_with(".cpp"))
            continue;
        runtime_dispatch_shards += content;
        ++runtime_dispatch_shard_count;
    }
    const auto unit =
        std::find_if(generated_before.begin(), generated_before.end(), [](const auto& entry) {
            return entry.first.starts_with("code/unit-00000-") && entry.first.ends_with(".cpp");
        });
    require(first.functions == 3u && first.partitions == 3u && first.checkpoints.size() == 8u &&
                first.checkpoints.back() == "port-project-written",
            "Synthetische GDI durchlaeuft den Portexport nicht vollstaendig.");
    const std::vector<std::string> expected_progress = {"disc-load",
                                                        "boot-image",
                                                        "control-flow-analysis",
                                                        "ir-lowering",
                                                        "ir-optimization",
                                                        "input-provenance",
                                                        "program-validation",
                                                        "partition-codegen",
                                                        "metadata",
                                                        "disc-recipe",
                                                        "artifact-write"};
    auto progress_cursor = observed_progress.cbegin();
    for (const auto& expected : expected_progress) {
        progress_cursor = std::find(progress_cursor, observed_progress.cend(), expected);
        require(progress_cursor != observed_progress.cend(),
                "Portexport verliert die Subphase " + expected + ".");
        ++progress_cursor;
    }
    require(std::any_of(observed_progress.begin(), observed_progress.end(), [](const auto& phase) {
                return phase.starts_with("control-flow-iteration-start-i1-");
            }) &&
                std::any_of(observed_progress.begin(),
                            observed_progress.end(),
                            [](const auto& phase) {
                                return phase.starts_with("control-flow-complete-");
                            }),
            "Portexport verliert budgetierte Kontrollfluss-Fixpunktzaehler.");
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
    const auto trace_enable = generated_main.find(
        "if (!enabled || runtime_wait_loop_descriptors.empty()) return;");
    const auto trace_allocate =
        generated_main.find("recorder_.emplace(runtime_wait_loop_descriptors)");
    const auto trace_set_sink =
        generated_main.find("set_guest_memory_access_sink(recorder_->sink())");
    const auto trace_clear_sink =
        generated_main.find("clear_guest_memory_access_sink()");
    const auto trace_serialize = generated_main.find("recorder_->serialize_json()");
    require(
        generated_main.find("#include \"katana/runtime/wait_loop_trace.hpp\"") !=
                std::string::npos &&
            generated_main.find(
                "std::array<katana::runtime::RuntimeWaitLoopDescriptor, 0u> "
                "runtime_wait_loop_descriptors") != std::string::npos &&
            generated_main.find(
                "std::optional<katana::runtime::RuntimeWaitLoopTraceRecorder> recorder_") !=
                std::string::npos &&
            generated_main.find(
                "std::getenv(\"KATANA_PORT_WAIT_LOOP_TRACE\")") != std::string::npos &&
            generated_main.find(
                "std::string_view(wait_loop_trace_value) == \"1\"") !=
                std::string::npos &&
            generated_main.find(
                "KATANA_WAIT_LOOP_TRACE_NOTICE local-only; contains raw guest-memory "
                "values; do not share without review") != std::string::npos &&
            generated_main.find("\"contains_raw_guest_values\\\":true") !=
                std::string::npos &&
            generated_main.find(
                "~RuntimeWaitLoopTraceSession() noexcept { finish(); }") !=
                std::string::npos &&
            generated_main.find("KATANA_WAIT_LOOP_TRACE ") != std::string::npos &&
            generated_main.find("katana.runtime-wait-loop-trace") !=
                std::string::npos &&
            trace_enable != std::string::npos && trace_allocate != std::string::npos &&
            trace_set_sink != std::string::npos && trace_clear_sink != std::string::npos &&
            trace_serialize != std::string::npos && trace_enable < trace_allocate &&
            trace_allocate < trace_set_sink && trace_clear_sink < trace_serialize &&
            generated_before.at("metadata/port-project.json")
                    .find("\"contract_version\":27") != std::string::npos &&
            generated_before.at("metadata/provenance.json")
                    .find("\"manifest_version\":27") != std::string::npos,
        "Portprodukt bindet den versionierten Wait-Loop-Trace nicht strikt opt-in, "
        "allokationsfrei im Normalpfad und RAII-bereinigt ein.");
    const auto poll_disc_directory = fixture.root / "poll-loop-disc";
    std::filesystem::create_directories(poll_disc_directory);
    write_fixture(poll_disc_directory);
    write_binary(poll_disc_directory / "high.bin", poll_loop_boot_track());
    const auto poll_output = fixture.root / "poll-loop-port";
    static_cast<void>(
        export_dreamcast_port_project(poll_disc_directory / "disc.gdi", poll_output, options));
    const auto poll_main = read_text(poll_output / "src" / "main.cpp");
    const auto conservative_descriptor = poll_main.find(
        "{0x8C010000u, 0x8C010000u, 0x8C010000u, "
        "katana::runtime::RuntimeWaitLoopEvidence::ConservativeCandidate}");
    const auto proven_descriptor = poll_main.find(
        "{0x8C010000u, 0x8C010000u, 0x8C010002u, "
        "katana::runtime::RuntimeWaitLoopEvidence::ProvenGuard}");
    static_cast<void>(
        export_dreamcast_port_project(poll_disc_directory / "disc.gdi", poll_output, options));
    require(
        poll_main.find(
            "std::array<katana::runtime::RuntimeWaitLoopDescriptor, 2u> "
            "runtime_wait_loop_descriptors") != std::string::npos &&
            conservative_descriptor != std::string::npos &&
            proven_descriptor != std::string::npos &&
            conservative_descriptor < proven_descriptor &&
            occurrences(poll_main, "RuntimeWaitLoopEvidence::ConservativeCandidate") == 1u &&
            occurrences(poll_main, "RuntimeWaitLoopEvidence::ProvenGuard") == 1u &&
            read_text(poll_output / "src" / "main.cpp") == poll_main &&
            poll_main.find(fixture.root.string()) == std::string::npos &&
            poll_main.find("poll-loop-disc") == std::string::npos &&
            poll_main.find("high.bin") == std::string::npos,
        "Generische Poll-Loops werden nicht deterministisch, dedupliziert oder frei von "
        "privaten Quelldaten in den Produkttrace exportiert.");
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
            constexpr std::string_view service_declaration =
                "_with_services(CpuState& cpu, PlatformServices* services);";
            std::size_t declaration_count = 0u;
            for (auto offset = content.find(service_declaration); offset != std::string::npos;
                 offset = content.find(service_declaration, offset + service_declaration.size()))
                ++declaration_count;
            require(declaration_count == 1u,
                    "Portpartition deklariert fremde Funktionen und skaliert quadratisch.");
            ++entry_metadata_count;
        }
    }
    require(entry_metadata_count == 3u && p2_pc_relative_literal,
            "Mehrteiliger Portexport verliert P2-Einstieg oder PC-relativen P2-Literalzugriff.");
    for (const auto& path : {"include/katana_port.hpp",
                              "include/runtime-dispatch-internal.hpp",
                              "code/runtime-dispatch.cpp",
                              "code/runtime-dispatch-shard-00000.cpp",
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
    require(runtime_dispatch_shard_count == 1u &&
                generated_before.at("CMakeLists.txt")
                        .find("code/runtime-dispatch-shard-00000.cpp") != std::string::npos &&
                generated_before.at(".katana-generated-artifacts")
                        .find("code/runtime-dispatch-shard-00000.cpp") != std::string::npos &&
                generated_before.at("code/runtime-dispatch.cpp")
                        .find("runtime_dispatch_detail::append_static_blocks_shard_00000") !=
                    std::string::npos &&
                generated_before.at("code/runtime-dispatch.cpp").find("generated-block-") ==
                    std::string::npos &&
                occurrences(runtime_dispatch_shards,
                            "BlockExit dispatch_owner_8C010000(") == 1u &&
                occurrences(runtime_dispatch_shards, "&dispatch_owner_8C010000") == 3u,
            "Runtime-Dispatch ist nicht deterministisch geshardet oder dupliziert Wrapper pro "
            "Block.");
    const auto runtime_probe_function =
        generated_main.find("int run_deterministic_runtime_probe(");
    const auto runtime_probe_function_end =
        generated_main.find("\nstd::string redact_source(", runtime_probe_function);
    const auto runtime_probe_replay_attach =
        generated_main.find("state.scheduler->attach_replay_log(replay)", runtime_probe_function);
    const auto runtime_probe_scheduler_coverage =
        generated_main.find("SystemReplayCoverage::SchedulerCallback",
                            runtime_probe_replay_attach);
    const auto runtime_probe_audio =
        generated_main.find("katana::runtime::RecordingHostAudioOutput audio",
                            runtime_probe_scheduler_coverage);
    const auto runtime_probe_clock =
        generated_main.find("katana::runtime::DreamcastMediaClock media_clock",
                             runtime_probe_audio);
    const auto runtime_probe_media_coverage =
        generated_main.find("SystemReplayCoverage::Video",
                            runtime_probe_clock);
    const auto runtime_probe_mmio_trace =
        generated_main.find("RuntimeProbeMmioTraceSession mmio_trace",
                            runtime_probe_media_coverage);
    const auto runtime_probe_input_coverage =
        generated_main.find("SystemReplayCoverage::Input",
                            runtime_probe_mmio_trace);
    const auto runtime_probe_input_source =
        generated_main.find("input->inject(1u, state.scheduler->current_cycle(), {})",
                            runtime_probe_input_coverage);
    const auto runtime_probe_input_event =
        generated_main.find("replay.inject({", runtime_probe_input_source);
    const auto runtime_probe_clock_start =
        generated_main.find("media_clock.start()", runtime_probe_input_event);
    const auto runtime_probe_dispatch = generated_main.find(
        "katana_port_generated::run_runtime(cpu, services, *state.runtime_blocks)",
        runtime_probe_clock_start);
    const auto runtime_probe_budget_catch = generated_main.find(
        "catch (const katana::runtime::RuntimeProbeBudgetReached& reached)",
        runtime_probe_dispatch);
    const auto runtime_probe_quiesce =
        generated_main.find("media_clock.stop();\n    mmio_trace.finish();",
                            runtime_probe_budget_catch);
    const auto runtime_probe_capture =
        generated_main.find("capture_runtime_probe_dreamcast(",
                            runtime_probe_quiesce);
    const auto runtime_probe_replay_seal =
        generated_main.find("replay.seal(provisional.hashes.guest_state)",
                            runtime_probe_capture);
    const auto runtime_probe_output =
        generated_main.find("std::cout << \"KATANA_RUNTIME_PROBE \"",
                            runtime_probe_replay_seal);
    const auto runtime_probe_serialize = generated_main.find(
        "serialize_runtime_probe_report_json(report)", runtime_probe_output);
    const auto runtime_probe_newline =
        generated_main.find("<< '\\n';", runtime_probe_serialize);
    const auto runtime_probe_return =
        generated_main.find("return 0;", runtime_probe_newline);
    const auto runtime_probe_branch =
        generated_main.find("if (deterministic_runtime_probe)");
    const auto runtime_probe_branch_call =
        generated_main.find("return run_deterministic_runtime_probe(",
                            runtime_probe_branch);
    const auto normal_native_video =
        generated_main.find("katana::runtime::native_video_available()",
                            runtime_probe_branch_call);
    const auto normal_native_audio =
        generated_main.find("katana::runtime::native_audio_available()",
                            normal_native_video);
    const auto normal_host_runtime =
        generated_main.find("katana::runtime::HostRuntimeSession host",
                            normal_native_audio);
    const auto normal_frame_pump =
        generated_main.find("const auto pump_guest_frame = [&] {",
                            normal_host_runtime);
    const auto normal_frame_proof =
        generated_main.find("pump_guest_frame_proof(", normal_frame_pump);
    const auto normal_runtime_dispatch = generated_main.find(
        "katana_port_generated::run_runtime(cpu, services, *state.runtime_blocks)",
        normal_frame_proof);
    const auto dispatch_probe_profile =
        runtime_dispatch.find("std::getenv(\"KATANA_RUNTIME_PROBE\")");
    const auto dispatch_probe_determinism =
        runtime_dispatch.find("materialization_policy.deterministic_no_host_time = true",
                              dispatch_probe_profile);
    const auto dispatch_probe_budget_catch = runtime_dispatch.find(
        "catch (const katana::runtime::RuntimeProbeBudgetReached&)");
    const auto dispatch_generic_catch =
        runtime_dispatch.find("catch (...)", dispatch_probe_budget_catch);
    require(
        generated_main.find("#include \"katana/runtime/runtime_probe.hpp\"") !=
                std::string::npos &&
            occurrences(generated_main,
                        "std::getenv(\"KATANA_RUNTIME_PROBE\")") == 1u &&
            generated_main.find(
                "std::string_view(value) == \"deterministic-v1\"") !=
                std::string::npos &&
            generated_main.find("runtime-probe-profile-invalid") !=
                std::string::npos &&
            generated_main.find(
                "std::string_view(diagnostics) != \"0\"") !=
                std::string::npos &&
            generated_main.find(
                "std::string_view(diagnostics) != \"1\"") !=
                std::string::npos &&
            generated_main.find(
                "guest_cycle_budget_from_environment().has_value()") !=
                std::string::npos &&
            generated_main.find("runtime-probe-budget-required") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_WAIT_LOOP_TRACE\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_DIAGNOSTICS_FULL\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_PROGRESS_INTERVAL\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_LIFECYCLE_TEST\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_BLOCK_LIMIT\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_IGNORE_FOCUS\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_MEMORY_PROBES\"") !=
                std::string::npos &&
            generated_main.find("runtime-probe-environment-conflict") !=
                std::string::npos,
        "Deterministische Runtime-Probe besitzt kein exaktes Profil oder keine "
        "geschlossene Umgebungsvalidierung.");
    require(
        runtime_probe_function != std::string::npos &&
            runtime_probe_function_end != std::string::npos &&
            runtime_probe_replay_attach != std::string::npos &&
            runtime_probe_scheduler_coverage != std::string::npos &&
            runtime_probe_audio != std::string::npos &&
            runtime_probe_clock != std::string::npos &&
            runtime_probe_media_coverage != std::string::npos &&
            runtime_probe_mmio_trace != std::string::npos &&
            runtime_probe_input_coverage != std::string::npos &&
            runtime_probe_input_source != std::string::npos &&
            runtime_probe_input_event != std::string::npos &&
            runtime_probe_clock_start != std::string::npos &&
            runtime_probe_dispatch != std::string::npos &&
            runtime_probe_budget_catch != std::string::npos &&
            runtime_probe_quiesce != std::string::npos &&
            runtime_probe_capture != std::string::npos &&
            runtime_probe_replay_seal != std::string::npos &&
            runtime_probe_output != std::string::npos &&
            runtime_probe_serialize != std::string::npos &&
            runtime_probe_newline != std::string::npos &&
            runtime_probe_return != std::string::npos &&
            runtime_probe_function < runtime_probe_replay_attach &&
            runtime_probe_replay_attach < runtime_probe_scheduler_coverage &&
            runtime_probe_scheduler_coverage < runtime_probe_audio &&
            runtime_probe_replay_attach < runtime_probe_audio &&
            runtime_probe_audio < runtime_probe_clock &&
            runtime_probe_clock < runtime_probe_media_coverage &&
            runtime_probe_media_coverage < runtime_probe_mmio_trace &&
            runtime_probe_clock < runtime_probe_mmio_trace &&
            runtime_probe_mmio_trace < runtime_probe_input_coverage &&
            runtime_probe_input_coverage < runtime_probe_input_source &&
            runtime_probe_input_source < runtime_probe_input_event &&
            runtime_probe_input_event < runtime_probe_clock_start &&
            runtime_probe_clock < runtime_probe_clock_start &&
            runtime_probe_clock_start < runtime_probe_dispatch &&
            runtime_probe_dispatch < runtime_probe_budget_catch &&
            runtime_probe_budget_catch < runtime_probe_quiesce &&
            runtime_probe_quiesce < runtime_probe_capture &&
            runtime_probe_capture < runtime_probe_replay_seal &&
            runtime_probe_replay_seal < runtime_probe_output &&
            runtime_probe_output < runtime_probe_serialize &&
            runtime_probe_serialize < runtime_probe_newline &&
            runtime_probe_newline < runtime_probe_return &&
            runtime_probe_return < runtime_probe_function_end &&
            generated_main.find("create_native_video_output",
                                runtime_probe_function) >
                runtime_probe_function_end &&
            generated_main.find("create_native_audio_output",
                                runtime_probe_function) >
                runtime_probe_function_end &&
            occurrences(generated_main,
                        "std::cout << \"KATANA_RUNTIME_PROBE \"") == 1u &&
            generated_main.find(
                "std::cerr << \"KATANA_RUNTIME_PROBE \"") ==
                std::string::npos &&
            generated_main.find(
                "throw katana::runtime::RuntimeProbeBudgetReached("
                "result.guest_cycle)") != std::string::npos &&
            generated_main.find(
                "termination = "
                "katana::runtime::RuntimeProbeTermination::BudgetReached",
                runtime_probe_budget_catch) != std::string::npos &&
             generated_main.find(
                 "report.guest_cycle != *report.guest_cycle_budget",
                 runtime_probe_budget_catch) != std::string::npos &&
            generated_main.find(
                "SystemReplayProfile::DeterministicV1",
                runtime_probe_function) != std::string::npos &&
            generated_main.find(
                "system_replay_mmio_observer(") != std::string::npos &&
            generated_main.find(
                "SystemReplayEventKind::Dma") != std::string::npos &&
            generated_main.find(
                "\"neutral-controller-input\"",
                runtime_probe_function) != std::string::npos &&
            occurrences(generated_main, "enable_coverage(") == 5u &&
            generated_main.find(
                "enable_coverage(replay.required_coverage())") == std::string::npos &&
            generated_main.find("SystemReplayCoverage::CpuSafepoint") !=
                std::string::npos &&
            generated_main.find("SystemReplayCoverage::AcceptedInterrupt") !=
                std::string::npos &&
            generated_main.find("SystemReplayCoverage::Dma") != std::string::npos &&
            generated_main.find("SystemReplayCoverage::Audio") != std::string::npos &&
            generated_main.find("SystemReplayCoverage::Mmio") != std::string::npos,
        "Runtime-Probe bindet Replay, typed Budget-Exit oder genau eine Ergebniszeile "
        "nicht deterministisch beziehungsweise quiesziert vor dem Seal nicht.");
    require(
        runtime_probe_branch != std::string::npos &&
            runtime_probe_branch_call != std::string::npos &&
            normal_native_video != std::string::npos &&
            normal_native_audio != std::string::npos &&
            normal_host_runtime != std::string::npos &&
            normal_frame_pump != std::string::npos &&
            normal_frame_proof != std::string::npos &&
            normal_runtime_dispatch != std::string::npos &&
            runtime_probe_branch < runtime_probe_branch_call &&
            runtime_probe_branch_call < normal_native_video &&
            normal_native_video < normal_native_audio &&
            normal_native_audio < normal_host_runtime &&
            normal_host_runtime < normal_frame_pump &&
            normal_frame_pump < normal_frame_proof &&
            normal_frame_proof < normal_runtime_dispatch &&
            dispatch_probe_profile != std::string::npos &&
            dispatch_probe_determinism != std::string::npos &&
            dispatch_probe_budget_catch != std::string::npos &&
            dispatch_generic_catch != std::string::npos &&
            dispatch_probe_profile < dispatch_probe_determinism &&
            dispatch_probe_budget_catch < dispatch_generic_catch,
        "Runtime-Probe liegt nicht vor nativen Hostbackends oder schwaecht den normalen "
        "Produkt-Frame-/Dispatchpfad ab.");
    require(
        generated_before.at("katana-port.cmake").find("add_executable(synthetic_game") !=
                std::string::npos &&
            generated_before.at("katana-port.cmake").find("katana_runtime") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("dispatch_indirect") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("execute_dynamic_sh4_block(cpu, *active_services, 1u)") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("dynamic_interpreter.hpp") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("dispatch_dynamic_interpreter") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("runtime-sh4-interpreter") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("materialization_policy.enabled = false") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("native_aot_template.hpp") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("NativeAotTemplateBinder native_aot_binder") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("target, physical_origin, bytes, variant)") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("materialization_policy, {}") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("count < 64u") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("candidate.instructions = 1u") == std::string::npos &&
            runtime_dispatch_shards
                    .find("generated-block-AC008300") != std::string::npos &&
            runtime_dispatch_shards
                    .find("register_executable_block(table, services, 0xAC008300u") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"execution_coverage_contract\":"
                          "\"native-aot-or-typed-abort-v1\"") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"dispatch_paths_without_validation\":0") != std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"execution_profile\":\"native-aot-product\"") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"runtime_interpreter_enabled\":false") != std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"unbound_code_policy\":\"typed-materialization-error\"") !=
                std::string::npos &&
            runtime_dispatch_shards.find("generated-block-8C010000") !=
                std::string::npos &&
            runtime_dispatch_shards
                    .find("register_executable_block(table, services, 0x8C010000u") !=
                std::string::npos &&
            runtime_dispatch_shards
                    .find("services.allow_executable_block_chaining(") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("executed_dispatch_blocks >= block_budget") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("KATANA_PORT_BLOCK_PROGRESS") !=
                std::string::npos &&
            runtime_dispatch_shards
                    .find("append_static_block(blocks, 0x8C010000u") != std::string::npos &&
            runtime_dispatch_shards.find("static_blocks.push_back({") ==
                std::string::npos &&
            runtime_dispatch_shards
                    .find("if (const auto registered_handle = table.lookup(0x8C010000u") ==
                std::string::npos &&
            runtime_dispatch_shards
                    .find("6u, katana::runtime::BlockEndKind::Call") != std::string::npos &&
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
                    .find("target = cpu.pc;\n"
                          "            dispatch_callsite = exit.source.virtual_address;\n"
                          "            dispatch_source = exit.source;\n"
                          "            kind = "
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
            read_text(output / "src" / "main.cpp").find("peek_guest_u32") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pvr_registers->snapshot()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("system_bus_control->snapshot()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pvr_registers->read(") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("system_bus_control->read(") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("std::array<const katana::runtime::MemoryDevice*, 3u>") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("state.flash.get()};") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu.memory.read_u32(address)") == std::string::npos &&
            read_text(output / "src" / "main.cpp").find("translate_guest_address") ==
                std::string::npos &&
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
                    .find("KATANA_PORT_BLOCK_LIMIT\") == nullptr &&") == std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("std::getenv(\"KATANA_PORT_PROGRESS_INTERVAL\") == nullptr") ==
                std::string::npos &&
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
                    .find("local_block_chain_guest_cycle_budget = 4'096u") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("pending_guest_cycles + found->second.maximum_guest_cycles") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("prospective_guest_cycles > *remaining") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("*event <= current_cycle + prospective_guest_cycles") !=
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
            read_text(output / "src" / "main.cpp")
                    .find("retired_guest_instructions=") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("register_index < cpu.r.size()") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu.r[register_index]") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.read_sr()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.read_fpscr()") !=
                std::string::npos &&
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
            normal_frame_proof < normal_runtime_dispatch &&
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
    auto diagnostic_options = options;
    diagnostic_options.diagnostic_partial = true;
    const auto diagnostic_output = fixture.root / "diagnostic-port";
    static_cast<void>(export_dreamcast_port_project(gdi, diagnostic_output, diagnostic_options));
    const auto diagnostic_dispatch =
        read_text(diagnostic_output / "generated" / "code" / "runtime-dispatch.cpp");
    require(diagnostic_dispatch.find("dynamic_interpreter.hpp") != std::string::npos &&
                diagnostic_dispatch.find(
                    "execute_dynamic_sh4_block(cpu, *active_services, 1u)") !=
                    std::string::npos &&
                diagnostic_dispatch.find("runtime-sh4-interpreter") != std::string::npos &&
                diagnostic_dispatch.find("candidate.interpreter_backed = true") !=
                    std::string::npos &&
                diagnostic_dispatch.find("materialization_policy.enabled = true") !=
                    std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"diagnostic_partial\":true") != std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"execution_profile\":\"diagnostic-interpreter\"") !=
                    std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"runtime_interpreter_enabled\":true") !=
                    std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"execution_coverage_contract\":"
                              "\"diagnostic-validated-demand-v1\"") !=
                    std::string::npos,
            "Explizites Diagnoseprofil besitzt keinen klar isolierten SH-4-Interpreter.");
    const auto retire_marker = runtime_dispatch.find(
        "const auto retired_before = cpu.retired_guest_instructions");
    const auto execute_marker = runtime_dispatch.find(
        "katana::runtime::execute_runtime_block(", retire_marker);
    const auto scheduler_marker =
        runtime_dispatch.find("active_services->consume_guest_cycles(", execute_marker);
    const auto interrupt_marker =
        runtime_dispatch.find("active_services->poll_interrupt().has_value()", scheduler_marker);
    require(retire_marker != std::string::npos && execute_marker != std::string::npos &&
                scheduler_marker != std::string::npos && interrupt_marker != std::string::npos &&
                retire_marker < execute_marker && execute_marker < scheduler_marker &&
                scheduler_marker < interrupt_marker,
            "Gastzyklen oder Interruptannahme liegen nicht hinter der ausgefuehrten Blocksemantik.");
    const auto fallthrough_stall_marker = runtime_dispatch.find(
        "exit.kind == katana::runtime::BlockEndKind::Fallthrough", execute_marker);
    const auto source_relative_marker = runtime_dispatch.find(
        "exit.source.virtual_address + 2u", execute_marker);
    const auto exact_target_marker = runtime_dispatch.find(
        "exit.target->virtual_address != expected_fallthrough", fallthrough_stall_marker);
    const auto stale_entry_comparison = runtime_dispatch.find("cpu.pc == block_entry_pc");
    require(source_relative_marker != std::string::npos &&
                fallthrough_stall_marker != std::string::npos &&
                exact_target_marker != std::string::npos &&
                stale_entry_comparison == std::string::npos &&
                execute_marker < source_relative_marker &&
                source_relative_marker < fallthrough_stall_marker &&
                fallthrough_stall_marker < exact_target_marker &&
                exact_target_marker < scheduler_marker,
            "Portdispatch prueft den exakten Fallthrough nicht relativ zur zuletzt ausgefuehrten "
            "Quellinstruktion oder verwechselt ihn mit dem Root-Blockeintritt.");
    const auto progress_marker =
        runtime_dispatch.find("executed_dispatch_blocks % progress_interval");
    const auto root_dispatch_marker = runtime_dispatch.find(
        "const auto selected = katana::runtime::dispatch_indirect", progress_marker);
    const auto root_begin_marker =
        runtime_dispatch.find("active_services->begin_executable_block", root_dispatch_marker);
    require(progress_marker != std::string::npos && root_dispatch_marker != std::string::npos &&
                root_begin_marker != std::string::npos && progress_marker < root_dispatch_marker &&
                root_dispatch_marker < root_begin_marker,
            "Portfortschritt liegt nicht am kontrollierten Root-Dispatch-Safepoint.");
    const auto callsite_state_marker =
        runtime_dispatch.find("std::uint32_t dispatch_callsite = cpu.pc");
    const auto attributed_request_marker = runtime_dispatch.find(
        "{kind, dispatch_callsite, target, cpu.pr, dispatch_source", callsite_state_marker);
    const auto site_reset_marker = runtime_dispatch.find(
        "active_exit_site_class = katana::runtime::DynamicDispatchSiteClass::NotDynamic",
        attributed_request_marker);
    const auto continuation_marker = runtime_dispatch.find(
        "make_indirect_dispatch_continuation", site_reset_marker);
    const auto continuation_callsite_marker = runtime_dispatch.find(
        "dispatch_callsite = continuation.callsite", continuation_marker);
    require(callsite_state_marker != std::string::npos &&
                attributed_request_marker != std::string::npos &&
                site_reset_marker != std::string::npos &&
                continuation_marker != std::string::npos &&
                continuation_callsite_marker != std::string::npos &&
                callsite_state_marker < attributed_request_marker &&
                attributed_request_marker < site_reset_marker &&
                site_reset_marker < continuation_marker &&
                continuation_marker < continuation_callsite_marker,
            "Dynamische Portkette verliert Terminator-Callsite oder RuntimeOnly-Klasse.");
    const auto require_chain_registration = [&](const std::string_view end_kind,
                                                const bool expected) {
        const auto marker = ", katana::runtime::BlockEndKind::" + std::string(end_kind);
        const auto marker_position = runtime_dispatch_shards.find(marker);
        require(marker_position != std::string::npos,
                "Portfixture besitzt keine Endklasse " + std::string(end_kind) + ".");
        const auto address_begin = runtime_dispatch_shards.rfind("0x", marker_position);
        const auto address_end = runtime_dispatch_shards.find('u', address_begin);
        require(address_begin != std::string::npos && address_end != std::string::npos,
                "Portfixture verliert die Blockadresse vor " + std::string(end_kind) + ".");
        const auto address =
            runtime_dispatch_shards.substr(address_begin, address_end - address_begin);
        const auto registration =
            "services.allow_executable_block_chaining(" + address + "u)";
        require((runtime_dispatch_shards.find(registration) != std::string::npos) == expected,
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
    const auto native_boot_image = katana::platform::make_dreamcast_disc_executable(
        guarded_disc,
        katana::platform::DreamcastDiscExecutionPath::NativeSystemBootstrap);
    require(native_boot_image.entry_points().size() == 2u &&
                native_boot_image.initial_snapshot_entry() ==
                    katana::platform::dreamcast_system_bootstrap_entry_address,
            "Native Disc-AOT-Wurzeln verlieren den ausgezeichneten Bootstrap-Snapshotentry.");
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
    const katana::ir::BasicBlock* runtime_only_block = nullptr;
    const katana::ir::BasicBlock* runtime_only_predecessor = nullptr;
    for (const auto& function : runtime_only_program) {
        for (const auto& block : function.blocks) {
            const auto site = std::find_if(
                block.instructions.begin(), block.instructions.end(), [&](const auto& instruction) {
                    return instruction.source_address ==
                           runtime_only_resolution.instruction_address;
                });
            if (site != block.instructions.end()) runtime_only_block = &block;
        }
    }
    require(runtime_only_block != nullptr,
            "Synthetische Runtime-only-Stelle besitzt keinen IR-Block.");
    for (const auto& function : runtime_only_program) {
        for (const auto& block : function.blocks) {
            if (std::find(block.successors.begin(),
                          block.successors.end(),
                          runtime_only_block->start_address) != block.successors.end())
                runtime_only_predecessor = &block;
        }
    }
    require(runtime_only_predecessor != nullptr,
            "Runtime-only-Fixture besitzt keinen statisch chainbaren Vorgaengerblock.");
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
    std::string runtime_only_dispatch_shards;
    for (const auto& [path, content] : runtime_only_sources)
        if (path.starts_with("code/unit-"))
            runtime_only_text += content;
        else if (path.starts_with("code/runtime-dispatch-shard-"))
            runtime_only_dispatch_shards += content;
    const auto block_symbol = hex_symbol(runtime_only_block->start_address);
    const auto note_function = runtime_only_dispatch_shards.find("bool note_block_entry_shard_");
    const auto mapping_begin =
        runtime_only_dispatch_shards.find("case 0x" + block_symbol + "u:", note_function);
    const auto mapping_end = runtime_only_dispatch_shards.find("return true;", mapping_begin);
    const auto mapping = mapping_begin != std::string::npos && mapping_end != std::string::npos
                             ? runtime_only_dispatch_shards.substr(mapping_begin,
                                                                   mapping_end - mapping_begin)
                             : std::string{};
    const auto site_symbol = hex_symbol(runtime_only_resolution.instruction_address);
    const auto expected_source =
        "active_exit_source = {katana::runtime::relocate_code_address(0x" + site_symbol +
        "u), katana::runtime::canonical_physical_address("
        "katana::runtime::relocate_code_address(0x" + site_symbol + "u))}";
    const auto predecessor_symbol = hex_symbol(runtime_only_predecessor->start_address);
    const auto predecessor_case =
        runtime_only_text.find("case 0x" + predecessor_symbol + "u: {");
    const auto predecessor_note = runtime_only_text.find(
        "note_block_entry(katana::runtime::relocate_code_address(0x" + predecessor_symbol +
            "u))",
        predecessor_case);
    const auto local_chain = runtime_only_text.find(
        "services->can_chain_executable_block(cpu.pc)) continue", predecessor_note);
    const auto runtime_only_case =
        runtime_only_text.find("case 0x" + block_symbol + "u: {", local_chain);
    const auto runtime_only_note = runtime_only_text.find(
        "note_block_entry(katana::runtime::relocate_code_address(0x" + block_symbol + "u))",
        runtime_only_case);
    require(runtime_only_text.find("runtime_only_jump") != std::string::npos &&
                predecessor_case != std::string::npos &&
                predecessor_note != std::string::npos && local_chain != std::string::npos &&
                runtime_only_case != std::string::npos &&
                runtime_only_note != std::string::npos && predecessor_case < predecessor_note &&
                predecessor_note < local_chain && local_chain < runtime_only_case &&
                runtime_only_case < runtime_only_note &&
                runtime_only_text.find(
                    "note_block_entry(katana::runtime::relocate_code_address(0x" + block_symbol +
                    "u))") !=
                    std::string::npos &&
                !mapping.empty() &&
                mapping.find("DynamicDispatchSiteClass::RuntimeOnly") != std::string::npos &&
                mapping.find("BlockEndKind::Call") != std::string::npos &&
                mapping.find(expected_source) != std::string::npos &&
                runtime_only_dispatch_shards.find("kind, active_exit_source") !=
                    std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":1") != std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Portexport verliert den validierenden Runtime-only-Vertrag.");

    const auto make_shard_program = [](const std::size_t count) {
        constexpr std::uint32_t base = katana::platform::dreamcast_disc_boot_address;
        katana::ir::Function function;
        function.entry_address = base;
        function.blocks.reserve(count);
        for (std::size_t index = 0u; index < count; ++index) {
            const auto address = base + static_cast<std::uint32_t>(index * 2u);
            katana::ir::Instruction instruction;
            instruction.source_address = address;
            instruction.original_opcode = 0x0009u;
            instruction.original_operation = katana::ir::Operation::Nop;
            instruction.operation = katana::ir::Operation::Nop;
            instruction.widths = katana::ir::operation_operand_widths(instruction.operation);
            instruction.status_effects =
                katana::ir::instruction_status_effects(instruction.operation);
            instruction.memory_effects =
                katana::ir::instruction_memory_effects(instruction.operation);
            instruction.accumulator_effects =
                katana::ir::operation_accumulator_effects(instruction.operation);
            katana::ir::BasicBlock block;
            block.start_address = address;
            block.instructions.push_back(instruction);
            if (index + 1u != count) block.successors.push_back(address + 2u);
            function.blocks.push_back(std::move(block));
        }
        return std::vector<katana::ir::Function>{std::move(function)};
    };
    katana::io::ExecutableImage shard_image(gdi);
    katana::io::ImageSegment shard_segment;
    shard_segment.name = "dispatch-shard-fixture";
    shard_segment.virtual_address = katana::platform::dreamcast_disc_boot_address;
    shard_segment.memory_size = 2048u;
    shard_segment.kind = katana::io::SegmentKind::Code;
    shard_segment.permissions = {true, false, true};
    shard_segment.bytes.resize(static_cast<std::size_t>(shard_segment.memory_size));
    for (std::size_t offset = 0u; offset < shard_segment.bytes.size(); offset += 2u)
        shard_segment.bytes[offset] = 0x09u;
    shard_segment.source_kind = katana::io::ImageSourceKind::DiscBootFile;
    shard_segment.load_phase = katana::io::ImageLoadPhase::Initial;
    shard_image.add_segment(std::move(shard_segment));
    shard_image.add_entry_point(katana::platform::dreamcast_disc_boot_address);
    katana::analysis::ControlFlowAnalysisResult shard_analysis;
    auto native_template_program = make_shard_program(33u);
    auto native_template_analysis = shard_analysis;
    constexpr auto native_template_source = katana::platform::dreamcast_disc_boot_address;
    constexpr auto native_template_patch_slot = native_template_source + 12u;
    constexpr auto native_template_handler = native_template_source + 0x40u;
    constexpr auto native_template_live_handler = native_template_handler + 0x20000000u;
    native_template_analysis.runtime_code_copies.copies.push_back(
        {native_template_source,
         native_template_source,
         native_template_source,
         native_template_source + 12u,
         16u,
         0x600,
         {{native_template_source,
           native_template_patch_slot,
           native_template_live_handler,
           native_template_handler}},
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         true,
         "synthetic bounded runtime copy"});
    const auto native_template_output = fixture.root / "native-template-port";
    static_cast<void>(export_dreamcast_port_project(
        {shard_image,
         native_template_analysis,
         native_template_program,
         guarded_inputs,
         native_template_source,
         native_template_source,
         2048u,
         "native-template-fixture"},
        native_template_output,
        options));
    const auto native_template_sources = snapshot(native_template_output / "generated");
    const auto& native_template_dispatch =
        native_template_sources.at("code/runtime-dispatch.cpp");
    require(native_template_dispatch.find("materialization_policy.enabled = true") !=
                    std::string::npos &&
                native_template_dispatch.find(
                    "dreamcast_initial_boot_executable_module_id), \"sha256:") !=
                    std::string::npos &&
                native_template_dispatch.find("{0xAC010040u,0x8C010040u}") !=
                    std::string::npos &&
                native_template_dispatch.find("{0x8C010040u,0x00000000u}") ==
                    std::string::npos,
            "Portexport verliert Rohzeiger/native Blockadresse oder aktiviert den nativen "
            "Templatebinder nicht.");
    const auto shard_output = fixture.root / "dispatch-shard-port";
    auto shard_program = make_shard_program(513u);
    static_cast<void>(export_dreamcast_port_project(
        {shard_image,
         shard_analysis,
         shard_program,
         guarded_inputs,
         katana::platform::dreamcast_disc_boot_address,
         katana::platform::dreamcast_disc_boot_address,
         24u,
         "dispatch-shard-fixture"},
        shard_output,
        options));
    const auto shard_sources = snapshot(shard_output / "generated");
    const auto& shard_core = shard_sources.at("code/runtime-dispatch.cpp");
    const auto& shard_zero = shard_sources.at("code/runtime-dispatch-shard-00000.cpp");
    const auto& shard_one = shard_sources.at("code/runtime-dispatch-shard-00001.cpp");
    require(!shard_sources.contains("code/runtime-dispatch-shard-00002.cpp") &&
                shard_core.find("if (source_address <= 0x8C0103FEu)") !=
                    std::string::npos &&
                shard_zero.find("case 0x8C0103FEu:") != std::string::npos &&
                shard_zero.find("case 0x8C010400u:") == std::string::npos &&
                shard_one.find("case 0x8C010400u:") != std::string::npos &&
                occurrences(shard_zero, "append_static_block(blocks,") == 512u &&
                occurrences(shard_one, "append_static_block(blocks,") == 1u &&
                occurrences(shard_zero, "BlockExit dispatch_owner_8C010000(") == 1u &&
                occurrences(shard_one, "BlockExit dispatch_owner_8C010000(") == 1u &&
                shard_sources.at("CMakeLists.txt")
                        .find("code/runtime-dispatch-shard-00001.cpp") != std::string::npos &&
                shard_sources.at(".katana-generated-artifacts")
                        .find("code/runtime-dispatch-shard-00001.cpp") != std::string::npos,
            "513 Bloecke werden nicht an der deterministischen 512er-Shardgrenze getrennt.");
    shard_program = make_shard_program(512u);
    const auto shrunk_shard_export = export_dreamcast_port_project(
        {shard_image,
         shard_analysis,
         shard_program,
         guarded_inputs,
         katana::platform::dreamcast_disc_boot_address,
         katana::platform::dreamcast_disc_boot_address,
         24u,
         "dispatch-shard-fixture"},
        shard_output,
        options);
    const auto shrunk_shard_sources = snapshot(shard_output / "generated");
    require(shrunk_shard_export.removed_files >= 1u &&
                !shrunk_shard_sources.contains("code/runtime-dispatch-shard-00001.cpp") &&
                shrunk_shard_sources.at("CMakeLists.txt")
                        .find("code/runtime-dispatch-shard-00001.cpp") == std::string::npos &&
                shrunk_shard_sources.at(".katana-generated-artifacts")
                        .find("code/runtime-dispatch-shard-00001.cpp") == std::string::npos,
            "Geschrumpfter Portexport entfernt veraltete Runtime-Dispatch-Shards nicht.");

    {
        std::ofstream user(output / "src" / "notes.txt", std::ios::trunc);
        user << "keep-user-file\n";
    }
    const auto second = export_dreamcast_port_project(gdi, output, options);
    require(generated_before == snapshot(output / "generated") &&
                read_text(output / "src" / "main.cpp") == generated_main &&
                second.removed_files == 0u,
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
    incomplete_track[payload_offset(21u, 4u)] = 0x09u;
    incomplete_track[payload_offset(21u, 5u)] = 0x00u;
    write_binary(fixture.root / "disc" / "high.bin", incomplete_track);
    const auto incomplete_output = fixture.root / "incomplete-port";
    static_cast<void>(export_dreamcast_port_project(gdi, incomplete_output, options));
    const auto inferred_runtime_sources = snapshot(incomplete_output / "generated");
    std::string inferred_runtime_text;
    for (const auto& [path, content] : inferred_runtime_sources)
        if (path.starts_with("code/unit-")) inferred_runtime_text += content;
    require(inferred_runtime_text.find("runtime_only_jump") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":1") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Allgemeiner Runtimezeiger erreicht den validierenden Portvertrag nicht; der "
            "ausgezeichnete Bootstrap-Snapshot muss separat statisch bleiben.");

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
