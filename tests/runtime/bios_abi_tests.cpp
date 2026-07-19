#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <cstdlib>
#include <iostream>
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
    const auto bootstrap_block = bootstrap_handle ? blocks.resolve(*bootstrap_handle) : std::nullopt;
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
    static_cast<void>(tracker.register_block(
        {stable_runtime_block_identity(static_block->get()),
         static_block->get().physical_origin,
         static_block->get().size,
         "generated-port",
         {},
         ExecutableBlockOrigin::ImageSegment}));
    blocks.bind_code_tracker(&tracker);
    require(vectors.size() == 6u && blocks.size() == 7u &&
                blocks.lookup(0x8C010000u, {}).has_value() &&
                handoff.runtime_symbols().size() == 12u,
            "BIOS-Bootstrap und nachfolgende statische AOT-Registry sind unvollstaendig.");
    for (const auto& vector : vectors)
        require(cpu.memory.read_u32(vector.slot_address) == vector.handler_address &&
                    cpu.memory.read_u16(vector.handler_address) == 0x000Bu &&
                    blocks.lookup_physical(vector.handler_address, {}).has_value(),
                "BIOS-ABI-Vektor, RAM-Stub oder physischer Dispatchalias fehlt.");
    require(handoff.resolve(0xAC000100u).statically_proven &&
                handoff.resolve(0xAC000100u).provenance == "hle-generated-handler",
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
    require(cpu.memory.read_u8(0x8C000068u) == 0xA0u &&
                cpu.memory.read_u8(0x8C00006Fu) == 0xA7u &&
                cpu.memory.read_u8(0x8C000070u) == '0' &&
                cpu.memory.read_u8(0x8C000077u) == 0u,
            "SYSINFO_INIT baut den 24-Byte-Systemblock nicht aus Flash und Nullpadding auf.");
    cpu.pc = vectors[0].handler_address;
    cpu.r[7] = 3u;
    static_cast<void>(init_block->get().function(cpu, context));
    require(cpu.r[0] == 0x8C000068u, "SYSINFO_ID liefert nicht den initialisierten Systemblock.");
    cpu.pc = vectors[0].handler_address;
    cpu.r[7] = 2u;
    cpu.r[4] = 9u;
    static_cast<void>(init_block->get().function(cpu, context));
    require(cpu.r[0] == 704u, "SYSINFO_ICON akzeptiert keinen gueltigen Iconindex.");
    cpu.pc = vectors[0].handler_address;
    cpu.r[7] = 2u;
    cpu.r[4] = 10u;
    static_cast<void>(init_block->get().function(cpu, context));
    require(cpu.r[0] == 0xFFFFFFFFu, "SYSINFO_ICON akzeptiert einen ungueltigen Iconindex.");

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
    require(cpu.r[0] == 0u && cpu.memory.read_u16(0x8C002100u) == 0x3412u,
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
    require(cpu.r[0] == 0u &&
                cpu.memory.read_u8(dreamcast_flash_physical_base + 0x10000u) == 0xFFu,
            "FLASHROM_DELETE loescht eine gueltige Partition nicht sektorweise.");
    cpu.r[4] = static_cast<std::uint32_t>(dreamcast_flash_size - 1u);
    cpu.r[5] = 0x8C002100u;
    cpu.r[6] = 2u;
    static_cast<void>(invoke_flash(1u));
    require(cpu.r[0] == 0xFFFFFFFFu,
            "FLASHROM_READ akzeptiert einen ueberlaufenden Flashbereich.");

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
    cpu.r[7] = 99u;
    try {
        static_cast<void>(route_hle_bios_abi_call(cpu));
        require(false, "Unbekannter BIOS-Aufruf wurde ignoriert.");
    } catch (const BiosAbiDispatchError& error) {
        require(std::string(error.what()).find("unknown-function") != std::string::npos &&
                    std::string(error.what()).find("selector=0x00000063") != std::string::npos,
                "Unbekannter BIOS-Aufruf besitzt keine stabile Diagnose.");
    }
    const auto json = format_hle_bios_abi_contract_json();
    require(json.find("\"schema\":\"katana-bios-abi\"") != std::string::npos &&
                json.find("\"selector_register\":\"r7\"") != std::string::npos &&
                json.find("\"romfont_selector_register\":\"r1\"") != std::string::npos &&
                json.find("0x8C0000BC") != std::string::npos,
            "Maschinenlesbarer BIOS-ABI-Vertrag ist unvollstaendig.");
    std::cout << "KR-4602 BIOS-ABI, dynamische Vektoren und RAM-Handoff erfolgreich.\n";
}
