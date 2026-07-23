#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/gdrom_controller.hpp"
#include "katana/runtime/holly_dma.hpp"
#include "katana/runtime/platform_services.hpp"

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
} // namespace

int main() {
    using namespace katana::runtime;
    CpuState cpu;
    cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(cpu.memory));
    std::vector<std::uint8_t> flash_bytes(dreamcast_flash_size, 0xFFu);
    flash_bytes[0x20u] = 0x12u;
    flash_bytes[0x21u] = 0x34u;
    flash_bytes[0x10000u] = 0x00u;
    for (std::uint32_t index = 0u; index < 8u; ++index)
        flash_bytes[0x1A056u + index] = static_cast<std::uint8_t>(0xA0u + index);
    for (std::uint32_t index = 0u; index < 5u; ++index)
        flash_bytes[0x1A000u + index] = static_cast<std::uint8_t>('0' + index);
    static_cast<void>(map_dreamcast_command_flash(cpu.memory, flash_bytes));
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    FirmwareHandoffMap handoff;
    handoff.map_segment(
        {"main-ram", FirmwareSegmentKind::Ram, 0x8C000000u, 0x0C000000u, 0x01000000u});
    install_hle_bios_abi(cpu.memory, blocks, handoff, {}, 42u, &tracker);
    const auto vectors = hle_bios_abi_vectors();
    const auto bootstrap_handle = blocks.lookup(vectors[0].handler_address, {});
    const auto bootstrap_block =
        bootstrap_handle ? blocks.resolve(*bootstrap_handle) : std::nullopt;
    require(bootstrap_block.has_value() && !bootstrap_block->get().runtime_registered,
            "Fester BIOS-Bootstrapblock ist nicht statisch dispatchbar.");
    const auto static_handles = blocks.register_static_bulk({{0x8C010000u,
                                                              0x0C010000u,
                                                              4u,
                                                              BlockEndKind::Return,
                                                              {},
                                                              bootstrap_block->get().function,
                                                              "generated-after-bios-bootstrap"}});
    const auto static_block = blocks.resolve(static_handles.front());
    require(static_block.has_value(), "Statischer AOT-Testblock ist nicht aufloesbar.");
    static_cast<void>(tracker.register_block({stable_runtime_block_identity(static_block->get()),
                                              static_block->get().physical_origin,
                                              static_block->get().size,
                                              "generated-port",
                                              {},
                                              ExecutableBlockOrigin::ImageSegment}));
    blocks.bind_code_tracker(&tracker);
    require(vectors.size() == 6u && blocks.size() == 8u &&
                blocks.lookup(0x8C010000u, {}).has_value() &&
                blocks.lookup(hle_bios_gdrom2_direct_alias_address, {}).has_value() &&
                handoff.runtime_symbols().size() == 13u,
            "BIOS-Bootstrap und nachfolgende statische AOT-Registry sind unvollstaendig.");
    for (const auto& vector : vectors)
        require(vector.handler_address != 0x8C000100u &&
                    vector.handler_address != 0x8C000400u &&
                    vector.handler_address != 0x8C000600u &&
                    cpu.memory.read_u32(vector.slot_address) == vector.handler_address &&
                    cpu.memory.read_u16(vector.handler_address) == 0x000Bu &&
                    blocks.lookup_physical(vector.handler_address, {}).has_value(),
                "BIOS-ABI-Vektor kollidiert mit einem SH-4-Exceptionvektor oder sein RAM-Stub "
                "fehlt.");
    require(cpu.memory.read_u32(0x8C002400u) == 0xFFFFFFFFu &&
                cpu.memory.read_u16(hle_bios_gdrom2_direct_alias_address) == 0x000Bu,
            "Definierter 0xFF-BIOS-RAM-Grundzustand oder direkter GD2-Alias fehlt.");
    const auto p2_handler =
        0xA0000000u | canonical_physical_address(vectors[0].handler_address);
    require(handoff.resolve(p2_handler).statically_proven &&
                handoff.resolve(p2_handler).provenance == "hle-generated-handler",
            "P1/P2-RAM-Alias verliert den dynamischen HLE-Handoff.");

    cpu.pc = vectors[0].handler_address;
    cpu.r[7] = 0u;
    const auto init = route_hle_bios_abi_call(cpu);
    require(init.service == "sysinfo-init" && init.status == BiosAbiServiceStatus::Completed,
            "Titelunabhaengiger SYSINFO-Init-Aufruf wird nicht ausgefuehrt.");
    cpu.pr = 0x8C010000u;
    BlockExecutionContext context;
    const auto init_handle = blocks.lookup(vectors[0].handler_address, {});
    const auto init_block = init_handle ? blocks.resolve(*init_handle) : std::nullopt;
    require(init_block.has_value(), "Installierter BIOS-ABI-Block ist nicht aufloesbar.");
    const auto init_exit = init_block->get().function(cpu, context);
    require(init_exit.kind == BlockEndKind::Return && cpu.pc == cpu.pr && cpu.r[0] == 0u,
            "Installierter BIOS-ABI-Runtimeblock kehrt nicht ueber den gemeinsamen Blockvertrag "
            "zurueck.");
    require(cpu.memory.read_u8(0x8C000068u) == 0xA0u && cpu.memory.read_u8(0x8C00006Fu) == 0xA7u &&
                cpu.memory.read_u8(0x8C000070u) == '0' && cpu.memory.read_u8(0x8C000077u) == 0u,
            "SYSINFO_INIT baut den 24-Byte-Systemblock nicht aus Flash und Nullpadding auf.");
    cpu.pc = vectors[0].handler_address;
    cpu.r[7] = 3u;
    static_cast<void>(init_block->get().function(cpu, context));
    require(cpu.r[0] == 0x8C000068u, "SYSINFO_ID liefert nicht den initialisierten Systemblock.");
    cpu.pc = vectors[0].handler_address;
    cpu.r[7] = 2u;
    cpu.r[4] = 9u;
    cpu.r[5] = 0x8C002000u;
    cpu.memory.write_u32(cpu.r[5], 0xA5A5A5A5u, CodeWriteSource::Copy);
    const auto icon = route_hle_bios_abi_call(cpu);
    require(icon.status == BiosAbiServiceStatus::ServiceUnavailable,
            "Nicht implementierter SYSINFO_ICON-Dienst behauptet weiterhin Erfolg.");
    try {
        static_cast<void>(init_block->get().function(cpu, context));
        require(false, "SYSINFO_ICON kehrt ohne geschriebene 704 Bytes erfolgreich zurueck.");
    } catch (const BiosAbiDispatchError& error) {
        require(std::string(error.what()).find("service-unavailable:sysinfo-icon") !=
                        std::string::npos &&
                    cpu.memory.read_u32(cpu.r[5]) == 0xA5A5A5A5u,
                "SYSINFO_ICON meldet keine stabile ServiceUnavailable-Grenze.");
    }

    const auto flash_handle = blocks.lookup(vectors[2].handler_address, {});
    const auto flash_block = flash_handle ? blocks.resolve(*flash_handle) : std::nullopt;
    require(flash_block.has_value(), "Flash-BIOS-ABI-Block ist nicht aufloesbar.");
    const auto invoke_flash = [&](const std::uint32_t selector) {
        cpu.pc = vectors[2].handler_address;
        cpu.pr = 0x8C010000u;
        cpu.r[7] = selector;
        return flash_block->get().function(cpu, context);
    };
    cpu.r[4] = 3u;
    cpu.r[5] = 0x8C002000u;
    static_cast<void>(invoke_flash(0u));
    require(cpu.r[0] == 0u && cpu.memory.read_u32(0x8C002000u) == 0x10000u &&
                cpu.memory.read_u32(0x8C002004u) == 0x8000u,
            "FLASHROM_INFO liefert nicht die Dreamcast-Gamepartition.");
    cpu.r[4] = 0x20u;
    cpu.r[5] = 0x8C002100u;
    cpu.r[6] = 2u;
    static_cast<void>(invoke_flash(1u));
    require(cpu.r[0] == 2u && cpu.memory.read_u16(0x8C002100u) == 0x3412u,
            "FLASHROM_READ kopiert keine Flashbytes in den Gastpuffer.");
    cpu.memory.write_u8(0x8C002200u, 0x0Fu);
    cpu.r[4] = 0x30u;
    cpu.r[5] = 0x8C002200u;
    cpu.r[6] = 1u;
    static_cast<void>(invoke_flash(2u));
    require(cpu.r[0] == 1u && cpu.memory.read_u8(dreamcast_flash_physical_base + 0x30u) == 0x0Fu,
            "FLASHROM_WRITE programmiert keine Flashbytes mit echter 1->0-Semantik.");
    cpu.r[4] = 0x10000u;
    static_cast<void>(invoke_flash(3u));
    require(cpu.r[0] == 0u && cpu.memory.read_u8(dreamcast_flash_physical_base + 0x10000u) == 0xFFu,
            "FLASHROM_DELETE loescht eine gueltige Partition nicht sektorweise.");
    cpu.memory.write_u8(0x8C002200u, 0x00u);
    const auto factory_before = cpu.memory.read_u8(dreamcast_flash_physical_base + 0x1A000u);
    cpu.r[4] = 0x1A000u;
    cpu.r[5] = 0x8C002200u;
    cpu.r[6] = 1u;
    static_cast<void>(invoke_flash(2u));
    require(cpu.r[0] == 0xFFFFFFFFu &&
                cpu.memory.read_u8(dreamcast_flash_physical_base + 0x1A000u) == factory_before,
            "FLASHROM_WRITE veraendert die schreibgeschuetzte Factory-Partition.");
    cpu.r[4] = 0x1A000u;
    static_cast<void>(invoke_flash(3u));
    require(cpu.r[0] == 0xFFFFFFFFu &&
                cpu.memory.read_u8(dreamcast_flash_physical_base + 0x1A000u) == factory_before,
            "FLASHROM_DELETE loescht die schreibgeschuetzte Factory-Partition.");
    cpu.r[4] = static_cast<std::uint32_t>(dreamcast_flash_size - 1u);
    cpu.r[5] = 0x8C002100u;
    cpu.r[6] = 2u;
    static_cast<void>(invoke_flash(1u));
    require(cpu.r[0] == 0xFFFFFFFFu, "FLASHROM_READ akzeptiert einen ueberlaufenden Flashbereich.");

    const auto system_handle = blocks.lookup(vectors[5].handler_address, {});
    const auto system_block = system_handle ? blocks.resolve(*system_handle) : std::nullopt;
    require(system_block.has_value(), "System-BIOS-ABI-Block ist nicht aufloesbar.");
    EventScheduler g1_scheduler;
    DreamcastG1BusController g1(
        g1_scheduler, {}, [](const auto, const auto, const auto) {}, {});
    g1.configure_bios_handoff(0x0C123400u);
    g1.write(0x04u, 0x0C004000u);
    g1.write(0x08u, 32u);
    g1.write(0x0Cu, 1u);
    g1.write(0x14u, 1u);
    g1.write(0x18u, 1u);
    static_cast<void>(g1_scheduler.advance_by(128u, 1u));
    require(g1.read(0xF4u) == 0x0C004020u,
            "G1-Testsetup erreicht keinen vom BIOS-Handoff abweichenden Livezaehler.");
    cpu.g1_bus = &g1;
    cpu.pc = vectors[5].handler_address;
    cpu.pr = 0x8C010000u;
    cpu.r[4] = 0u;
    cpu.r[7] = 0xFF00001Cu;
    const auto system_init = route_hle_bios_abi_call(cpu);
    require(system_init.selector == 0u && system_init.service == "system-normal-init" &&
                system_init.status == BiosAbiServiceStatus::Completed,
            "Systemvektor liest den Funktionsselektor nicht aus r4.");
    static_cast<void>(system_block->get().function(cpu, context));
    require(cpu.r[0] == 0x00C0BEBCu && cpu.pc == cpu.pr &&
                g1.read(0x04u) == 0x0C004000u && g1.read(0x08u) == 32u &&
                g1.read(0xF4u) == 0x0C123400u && g1.read(0xF8u) == 0u,
            "System-Normalinitialisierung stellt Border oder getrennten G1-Livezaehler nicht "
            "wieder her.");
    cpu.pc = vectors[5].handler_address;
    cpu.r[4] = 2u;
    const auto check_disc = route_hle_bios_abi_call(cpu);
    require(check_disc.service == "system-check-disc" &&
                check_disc.status == BiosAbiServiceStatus::Completed,
            "System-Disc-Check ist nicht als bekannte Funktion geroutet.");
    static_cast<void>(system_block->get().function(cpu, context));
    require(cpu.r[0] == 0xFFFFFFFFu,
            "System-Disc-Check meldet ohne angebundenes Laufwerk ein Medium.");

    cpu.memory.write_u32(0x8C002400u, 0xC001D00Du, CodeWriteSource::Copy);
    cpu.memory.write_u32(dreamcast_disc_boot_address, 0xDEADBEEFu, CodeWriteSource::Copy);
    cpu.pc = vectors[5].handler_address;
    cpu.pr = 0x8C123456u;
    cpu.r[4] = 1u;
    cpu.gbr = 0xDEADBEEFu;
    context.scheduler_cycle = 12345u;
    context.sync_point = BlockSyncPoint::Entry;
    context.delay_slot_owner_pc = 0x8C000222u;
    const auto bios_menu = route_hle_bios_abi_call(cpu);
    require(bios_menu.service == "system-bios-menu" &&
                bios_menu.status == BiosAbiServiceStatus::Completed,
            "SYSTEM 1 ist nicht als nicht zurueckkehrendes BIOS-Menue geroutet.");
    try {
        static_cast<void>(system_block->get().function(cpu, context));
        require(false, "SYSTEM 1 kehrt in den Gast zurueck.");
    } catch (const PlatformLifecycleExit& exit) {
        const auto& evidence = exit.evidence();
        require(exit.reason() == PlatformLifecycleExitReason::BiosMenu &&
                    evidence.guest_cycle == 12345u && evidence.callsite == vectors[5].handler_address &&
                    evidence.return_address == 0x8C123456u && evidence.registers[4] == 1u &&
                    cpu.pc == vectors[5].handler_address && cpu.gbr == 0xDEADBEEFu &&
                    cpu.memory.read_u32(0x8C002400u) == 0xC001D00Du &&
                    cpu.memory.read_u32(dreamcast_disc_boot_address) == 0xDEADBEEFu,
                "SYSTEM 1 verliert Lifecycle-Evidenz oder mutiert den laufenden Gastzustand.");
    }

    cpu.pc = hle_bios_gdrom2_direct_alias_address;
    cpu.r[6] = 0u;
    cpu.r[7] = 0u;
    const auto direct_gd2 = route_hle_bios_abi_call(cpu);
    require(direct_gd2.vector == BiosAbiVectorKind::Gdrom2 &&
                direct_gd2.service == "gdrom2-undocumented" &&
                blocks.lookup(hle_bios_gdrom2_direct_alias_address, {}).has_value(),
            "Bekannter direkter GD2-BIOS-Einstieg 0x8C0010F0 ist nicht dispatchbar.");

    cpu.pc = vectors[3].handler_address;
    cpu.r[6] = 0u;
    cpu.r[7] = 3u;
    const auto gdrom = route_hle_bios_abi_call(cpu);
    require(gdrom.service == "gdrom-service" &&
                gdrom.status == BiosAbiServiceStatus::ServiceUnavailable,
            "Bekannter GD-ROM-Aufruf wird still erfolgreich gemeldet.");
    try {
        const auto gdrom_handle = blocks.lookup(vectors[3].handler_address, {});
        const auto gdrom_block = gdrom_handle ? blocks.resolve(*gdrom_handle) : std::nullopt;
        require(gdrom_block.has_value(), "GD-ROM-BIOS-ABI-Block ist nicht aufloesbar.");
        static_cast<void>(gdrom_block->get().function(cpu, context));
        require(false, "Nicht angebundener GD-ROM-Dienst lief als Erfolg weiter.");
    } catch (const BiosAbiDispatchError& error) {
        require(std::string(error.what()).find("service-unavailable:gdrom-service") !=
                    std::string::npos,
                "Runtimeblock meldet fehlenden Plattformdienst nicht konkret.");
    }

    const auto gdrom_handle = blocks.lookup(vectors[3].handler_address, {});
    const auto gdrom_block = gdrom_handle ? blocks.resolve(*gdrom_handle) : std::nullopt;
    require(gdrom_block.has_value(),
            "GD-ROM-Callback-Regression findet den echten BIOS-ABI-Block nicht.");
    EventScheduler callback_scheduler;
    std::vector<std::uint8_t> callback_disc_bytes(2u * 2048u);
    for (std::size_t index = 0u; index < callback_disc_bytes.size(); ++index)
        callback_disc_bytes[index] = static_cast<std::uint8_t>((index * 29u + 7u) & 0xFFu);
    auto callback_disc =
        std::make_shared<MemoryDiscSource>(callback_disc_bytes, "synthetic-bios-callback");
    DreamcastGdRomController callback_gdrom(
        cpu.memory,
        callback_scheduler,
        GdRomDrive(std::move(callback_disc)),
        {},
        {},
        {},
        {},
        {},
        DiscLoadExecutionPolicy::StandaloneTestMode);
    DreamcastG1BusController callback_g1(
        callback_scheduler,
        HollyDmaTiming{4u},
        [&](const std::uint32_t address,
            const std::uint32_t length,
            const std::uint32_t direction) {
            callback_gdrom.dma_to_memory(address, length, direction);
        },
        {});
    callback_gdrom.bind_g1_bus(&callback_g1);
    cpu.gdrom_services = &callback_gdrom;

    constexpr std::uint32_t callback_parameters = 0x8C003000u;
    constexpr std::uint32_t callback_transfer = 0x8C003020u;
    constexpr std::uint32_t callback_destination = 0x8C004000u;
    constexpr std::uint32_t callback_address = 0x8C010280u;
    constexpr std::uint32_t callback_argument = 0x2468ACE0u;
    constexpr std::uint32_t callback_pr = 0x8C012340u;
    const auto invoke_gdrom_block = [&](const std::uint32_t selector) {
        cpu.pc = vectors[3].handler_address;
        cpu.r[6] = 0u;
        cpu.r[7] = selector;
        return gdrom_block->get().function(cpu, context);
    };

    cpu.memory.write_u32(callback_parameters, 150u);
    cpu.memory.write_u32(callback_parameters + 4u, 2u);
    cpu.memory.write_u32(callback_parameters + 8u, 0u);
    cpu.memory.write_u32(callback_parameters + 12u, 0u);
    cpu.pr = callback_pr;
    cpu.r[4] = 28u;
    cpu.r[5] = callback_parameters;
    const auto queue_stream_exit = invoke_gdrom_block(0u);
    const auto callback_request = cpu.r[0];
    require(queue_stream_exit.kind == BlockEndKind::Return && callback_request >= 1u,
            "Command-28-DMA-Stream wird nicht ueber den echten BIOS-Block eingereiht.");
    cpu.r[4] = 0u;
    cpu.r[5] = 0u;
    static_cast<void>(invoke_gdrom_block(2u));
    static_cast<void>(callback_scheduler.advance_by(1'000u, 1u));

    const auto complete_dma_chunk = [&](const std::uint32_t destination) {
        cpu.memory.write_u32(callback_transfer, destination);
        cpu.memory.write_u32(callback_transfer + 4u, 2048u);
        cpu.pr = callback_pr;
        cpu.r[4] = callback_request;
        cpu.r[5] = callback_transfer;
        const auto start = invoke_gdrom_block(6u);
        require(start.kind == BlockEndKind::Return && cpu.r[0] == 0u &&
                    callback_g1.state().active == 1u,
                "Selector 6 startet den synthetischen Command-28-DMA-Chunk nicht.");
        static_cast<void>(callback_scheduler.advance_by(8'192u, 128u));
        require(callback_g1.state().active == 0u &&
                    callback_gdrom.status().completed_dma != 0u,
                "Synthetischer Command-28-DMA-Chunk erreicht keine Gastzeit-Completion.");
    };

    complete_dma_chunk(callback_destination);
    cpu.pr = callback_pr;
    cpu.r[4] = callback_address;
    cpu.r[5] = callback_argument;
    const auto callback_exit = invoke_gdrom_block(5u);
    require(callback_exit.kind == BlockEndKind::Call && cpu.pc == callback_address &&
                cpu.r[4] == callback_argument && cpu.pr == callback_pr,
            "Abgeschlossener Selector-5-Handoff liefert keinen Call mit Callbackargument und "
            "unveraendertem PR.");

    complete_dma_chunk(callback_destination + 0x1000u);
    cpu.pr = callback_pr;
    cpu.r[4] = callback_address | 1u;
    cpu.r[5] = callback_argument ^ 0xFFFFFFFFu;
    try {
        static_cast<void>(invoke_gdrom_block(5u));
        require(false, "Ungerade GD-ROM-DMA-Callbackadresse wurde dispatcht.");
    } catch (const BiosAbiDispatchError& error) {
        require(std::string(error.what()).find("invalid-transfer-callback") !=
                        std::string::npos &&
                    cpu.pr == callback_pr,
                "Ungerade GD-ROM-DMA-Callbackadresse besitzt keine kontrollierte "
                "BIOS-ABI-Diagnose.");
    }
    cpu.gdrom_services = nullptr;

    cpu.r[7] = 99u;
    try {
        static_cast<void>(route_hle_bios_abi_call(cpu));
        require(false, "Unbekannter BIOS-Aufruf wurde ignoriert.");
    } catch (const BiosAbiDispatchError& error) {
        require(std::string(error.what()).find("unknown-function") != std::string::npos &&
                    std::string(error.what()).find("selector=0x00000063") != std::string::npos,
                "Unbekannter BIOS-Aufruf besitzt keine stabile Diagnose.");
    }
    cpu.memory.write_u16(vectors[2].handler_address, 0x0009u);
    cpu.pc = vectors[2].handler_address;
    try {
        static_cast<void>(flash_block->get().function(cpu, context));
        require(false,
                "Mutierte HLE-Handlerbytes bleiben ueber einen alten Funktionshandle "
                "ausfuehrbar.");
    } catch (const BiosAbiDispatchError& error) {
        require(std::string(error.what()).find("handler-bytes-modified") != std::string::npos,
                "Mutierte HLE-Handlerbytes besitzen keine stabile Ablehnungsdiagnose.");
    }

    const auto json = format_hle_bios_abi_contract_json();
    require(json.find("\"schema\":\"katana-bios-abi\"") != std::string::npos &&
                json.find("\"selector_register\":\"r7\"") != std::string::npos &&
                json.find("\"romfont_selector_register\":\"r1\"") != std::string::npos &&
                json.find("\"system_selector_register\":\"r4\"") != std::string::npos &&
                json.find("0x8C0000BC") != std::string::npos &&
                json.find("0x8C0010F0") != std::string::npos,
            "Maschinenlesbarer BIOS-ABI-Vertrag ist unvollstaendig.");
    std::cout << "KR-4602 BIOS-ABI, dynamische Vektoren und RAM-Handoff erfolgreich.\n";
}
