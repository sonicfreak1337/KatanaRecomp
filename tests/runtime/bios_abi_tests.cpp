#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <cstdlib>
#include <iostream>
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
    using namespace katana::runtime;
    CpuState cpu;
    cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(cpu.memory));
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
