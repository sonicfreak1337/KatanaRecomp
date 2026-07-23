#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/hardware_audit.hpp"

#include <algorithm>
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

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    for (std::uint32_t shift = 0u; shift < 32u; shift += 8u)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void write_u16(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1u) = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint32_t value) {
    for (std::uint32_t shift = 0u; shift < 32u; shift += 8u)
        bytes.at(offset + shift / 8u) = static_cast<std::uint8_t>(value >> shift);
}

katana::analysis::DreamcastHardwareAudit
audit_fixture(const std::string& name, std::vector<std::uint8_t> bytes) {
    katana::io::ExecutableImage image(name);
    image.add_segment({".text",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       std::move(bytes)});
    image.add_entry_point(0u);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    return katana::analysis::audit_dreamcast_hardware(image, analysis);
}

std::vector<std::uint8_t> backward_poll_fixture(const std::uint32_t polled_address) {
    std::vector<std::uint8_t> bytes(0x1Cu, 0u);
    write_u16(bytes, 0x00u, 0xA002u); // BRA 0x08
    write_u16(bytes, 0x02u, 0x0009u); // NOP
    write_u16(bytes, 0x08u, 0xD103u); // MOV.L @(0x18,PC),R1
    write_u16(bytes, 0x0Au, 0x6012u); // MOV.L @R1,R0
    write_u16(bytes, 0x0Cu, 0x2008u); // TST R0,R0
    write_u16(bytes, 0x0Eu, 0x8901u); // BT 0x14
    write_u16(bytes, 0x10u, 0xAFFAu); // BRA 0x08
    write_u16(bytes, 0x12u, 0x0009u); // NOP
    write_u16(bytes, 0x14u, 0x000Bu); // RTS
    write_u16(bytes, 0x16u, 0x0009u); // NOP
    write_u32(bytes, 0x18u, polled_address);
    return bytes;
}

} // namespace

int main() {
    // Six PC-relative literal loads feed concrete MMIO accesses. The first address is used with
    // both a valid longword and a rejected byte width so aggregation must retain the weaker result.
    std::vector<std::uint8_t> bytes{0x07u, 0xD1u, 0x00u, 0xE0u, 0x02u, 0x21u, 0x07u, 0xD1u,
                                    0x00u, 0x21u, 0x07u, 0xD1u, 0x02u, 0x21u, 0x07u, 0xD1u,
                                    0x02u, 0x21u, 0x07u, 0xD1u, 0x02u, 0x21u, 0x07u, 0xD1u,
                                    0x02u, 0x21u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
    append_u32(bytes, 0xA05F6800u);
    append_u32(bytes, 0xA05F6800u);
    append_u32(bytes, 0xA05F6820u);
    append_u32(bytes, 0xA05F8048u);
    append_u32(bytes, 0xFFE00000u);
    append_u32(bytes, 0xA05F6900u);

    katana::io::ExecutableImage image("synthetic-hardware-audit");
    image.add_segment({".text",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       std::move(bytes)});
    image.add_entry_point(0u);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    const auto audit = katana::analysis::audit_dreamcast_hardware(image, analysis);

    require(audit.addresses.size() == 5u && audit.implemented_addresses == 1u &&
                audit.partial_addresses == 1u && audit.known_gap_addresses == 1u &&
                audit.rejected_addresses == 1u && audit.unmapped_addresses == 1u,
            "Allgemeine Hardware-Capabilityklassen wurden nicht vollstaendig erkannt.");
    require(audit.memory_access_sites == 6u && audit.resolved_memory_access_sites == 6u &&
                audit.unresolved_memory_access_sites == 0u,
            "Statische Auditabdeckung zaehlt aufgeloeste Speicherstellen falsch.");
    require(audit.addresses.front().guest_address == 0xA05F6800u &&
                audit.addresses.front().runtime_support ==
                    katana::analysis::HardwareRuntimeSupport::Rejected &&
                audit.addresses.front().widths == std::vector<std::uint8_t>({1u, 4u}),
            "Gemischte Zugriffsbreiten behalten nicht die schwaechste Runtime-Capability.");

    const auto json = katana::analysis::format_hardware_audit_json(audit);
    require(json.find("\"schema\":\"katana.hardware-audit.v3\"") != std::string::npos &&
                json.find("\"support_reason\":\"sort_dma_transfer_path_missing\"") !=
                    std::string::npos &&
                json.find("\"runtime_support\":\"unmapped\"") != std::string::npos,
            "Hardware-Audit-JSON verliert Schema oder maschinenlesbare Lueckengruende.");

    std::vector<std::uint8_t> exception_bytes{
        0x01u, 0xD1u, 0x12u, 0x60u, 0x0Bu, 0x00u, 0x09u, 0x00u};
    append_u32(exception_bytes, 0x1F000024u);
    katana::io::ExecutableImage exception_image("synthetic-sh4-exception-audit");
    exception_image.add_segment({".text",
                                 0u,
                                 0u,
                                 exception_bytes.size(),
                                 katana::io::SegmentKind::Code,
                                 {true, false, true},
                                 std::move(exception_bytes)});
    exception_image.add_entry_point(0u);
    const auto exception_analysis = katana::analysis::analyze_control_flow(exception_image);
    const auto exception_audit =
        katana::analysis::audit_dreamcast_hardware(exception_image, exception_analysis);
    require(exception_audit.addresses.size() == 1u && exception_audit.implemented_addresses == 1u &&
                exception_audit.addresses.front().canonical_address == 0xFF000024u &&
                exception_audit.addresses.front().region ==
                    katana::analysis::DreamcastHardwareRegion::Sh4Exception &&
                exception_audit.addresses.front().register_name == "EXPEVT",
            "SH-4-Exceptionregister oder Area-7-Alias bleiben im Auditor ungemappt.");

    const auto audit_rtc_read = [](const std::uint32_t address, const std::uint16_t opcode) {
        std::vector<std::uint8_t> rtc_bytes{0x01u,
                                            0xD1u,
                                            static_cast<std::uint8_t>(opcode),
                                            static_cast<std::uint8_t>(opcode >> 8u),
                                            0x0Bu,
                                            0x00u,
                                            0x09u,
                                            0x00u};
        append_u32(rtc_bytes, address);
        katana::io::ExecutableImage rtc_image("synthetic-aica-rtc-audit");
        rtc_image.add_segment({".text",
                               0u,
                               0u,
                               rtc_bytes.size(),
                               katana::io::SegmentKind::Code,
                               {true, false, true},
                               std::move(rtc_bytes)});
        rtc_image.add_entry_point(0u);
        const auto rtc_analysis = katana::analysis::analyze_control_flow(rtc_image);
        return katana::analysis::audit_dreamcast_hardware(rtc_image, rtc_analysis);
    };
    const auto valid_rtc = audit_rtc_read(0xA0710000u, 0x6012u);
    require(valid_rtc.addresses.size() == 1u && valid_rtc.implemented_addresses == 1u &&
                valid_rtc.addresses.front().canonical_address == 0x00710000u &&
                valid_rtc.addresses.front().register_name == "RTC_HIGH",
            "AICA-RTC-Register und direkter Alias werden nicht als implementiert erkannt.");
    const auto invalid_rtc = audit_rtc_read(0xA0710002u, 0x6011u);
    require(invalid_rtc.addresses.size() == 1u && invalid_rtc.rejected_addresses == 1u &&
                invalid_rtc.addresses.front().register_name.empty(),
            "AICA-RTC-Zwischenadresse wird vom Auditor faelschlich als Register behauptet.");
    const auto on_chip_ram = audit_rtc_read(0x7E001000u, 0x6012u);
    require(on_chip_ram.addresses.size() == 1u && on_chip_ram.implemented_addresses == 1u &&
                on_chip_ram.addresses.front().canonical_address == 0x7E001000u &&
                on_chip_ram.addresses.front().region ==
                    katana::analysis::DreamcastHardwareRegion::Sh4OnChipRam,
            "SH-4-On-Chip-RAM wird im allgemeinen Hardwareauditor falsch kanonisiert.");

    std::vector<std::uint8_t> forward_counter_bytes(0x14u, 0u);
    write_u16(forward_counter_bytes, 0x00u, 0xA006u); // BRA 0x10
    write_u16(forward_counter_bytes, 0x02u, 0x0009u); // NOP
    write_u16(forward_counter_bytes, 0x08u, 0x4010u); // DT R0
    write_u16(forward_counter_bytes, 0x0Au, 0x8B01u); // BF 0x10
    write_u16(forward_counter_bytes, 0x0Cu, 0x000Bu); // RTS
    write_u16(forward_counter_bytes, 0x0Eu, 0x0009u); // NOP
    write_u16(forward_counter_bytes, 0x10u, 0xAFFAu); // BRA 0x08
    write_u16(forward_counter_bytes, 0x12u, 0x0009u); // NOP
    const auto forward_counter =
        audit_fixture("forward-layout-counter-loop", std::move(forward_counter_bytes));
    require(forward_counter.loops.size() == 1u &&
                forward_counter.loops.front().header_address == 0x10u &&
                forward_counter.loops.front().latch_address == 0x08u &&
                forward_counter.loops.front().backedge_instruction_address == 0x0Au &&
                forward_counter.loops.front().block_addresses ==
                    std::vector<std::uint32_t>({0x08u, 0x10u}) &&
                forward_counter.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Counter &&
                forward_counter.loops.front().counter_instruction_addresses ==
                    std::vector<std::uint32_t>({0x08u}),
            "Vorwaerts angeordnete echte Backedge wird nicht per Dominanz als Counterloop erkannt.");

    const auto ram_poll =
        audit_fixture("backward-layout-ram-poll", backward_poll_fixture(0x0F001000u));
    require(ram_poll.loops.size() == 1u && ram_poll.loops.front().header_address == 0x08u &&
                ram_poll.loops.front().latch_address == 0x10u &&
                ram_poll.loops.front().backedge_instruction_address == 0x10u &&
                ram_poll.loops.front().block_addresses ==
                    std::vector<std::uint32_t>({0x08u, 0x10u}) &&
                ram_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::RamPoll &&
                ram_poll.loops.front().accesses.size() == 1u &&
                ram_poll.loops.front().accesses.front().linear_memory &&
                ram_poll.loops.front().accesses.front().canonical_address == 0x0C001000u &&
                ram_poll.loops.front().accesses.front().guards_loop,
            "Rueckwaerts angeordnete RAM-Pollbackedge oder Area-3-Spiegelkanonisierung fehlt.");

    const auto mmio_poll =
        audit_fixture("backward-layout-mmio-poll", backward_poll_fixture(0xA05F8000u));
    require(mmio_poll.loops.size() == 1u &&
                mmio_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::MmioPoll &&
                mmio_poll.loops.front().accesses.size() == 1u &&
                mmio_poll.loops.front().accesses.front().region ==
                    katana::analysis::DreamcastHardwareRegion::Pvr &&
                !mmio_poll.loops.front().accesses.front().linear_memory &&
                mmio_poll.loops.front().accesses.front().guards_loop,
            "MMIO-Polling wird nicht von linearem RAM-Polling getrennt.");

    const auto unmapped_poll =
        audit_fixture("unmapped-p4-poll", backward_poll_fixture(0xFF123456u));
    require(unmapped_poll.loops.size() == 1u &&
                unmapped_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                unmapped_poll.loops.front().accesses.size() == 1u &&
                unmapped_poll.loops.front().accesses.front().region ==
                    katana::analysis::DreamcastHardwareRegion::Sh4P4 &&
                !unmapped_poll.loops.front().accesses.front().aperture_mapped &&
                unmapped_poll.loops.front().accesses.front().runtime_support ==
                    katana::analysis::HardwareRuntimeSupport::Unmapped &&
                unmapped_poll.loops.front().accesses.front().guards_loop,
            "Nicht gemappter SH-4-P4-Zugriff wird faelschlich als MMIO-Polling behauptet.");

    std::vector<std::uint8_t> mixed_bytes(0x2Cu, 0u);
    write_u16(mixed_bytes, 0x00u, 0xA002u); // BRA 0x08
    write_u16(mixed_bytes, 0x02u, 0x0009u); // NOP
    write_u16(mixed_bytes, 0x08u, 0xD107u); // MOV.L @(0x28,PC),R1
    write_u16(mixed_bytes, 0x0Au, 0x6012u); // MOV.L @R1,R0
    write_u16(mixed_bytes, 0x0Cu, 0xC901u); // AND #1,R0
    write_u16(mixed_bytes, 0x0Eu, 0x6203u); // MOV R0,R2
    write_u16(mixed_bytes, 0x10u, 0x2228u); // TST R2,R2
    write_u16(mixed_bytes, 0x12u, 0x8907u); // BT 0x24
    write_u16(mixed_bytes, 0x14u, 0x4310u); // DT R3
    write_u16(mixed_bytes, 0x16u, 0x8BF7u); // BF 0x08
    write_u16(mixed_bytes, 0x18u, 0x000Bu); // RTS
    write_u16(mixed_bytes, 0x1Au, 0x0009u); // NOP
    write_u16(mixed_bytes, 0x24u, 0x000Bu); // RTS
    write_u16(mixed_bytes, 0x26u, 0x0009u); // NOP
    write_u32(mixed_bytes, 0x28u, 0x0C002000u);
    const auto mixed = audit_fixture("mixed-counter-poll-loop", std::move(mixed_bytes));
    require(mixed.loops.size() == 1u &&
                mixed.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Mixed &&
                mixed.loops.front().counter_instruction_addresses ==
                    std::vector<std::uint32_t>({0x14u}) &&
                mixed.loops.front().accesses.size() == 1u &&
                mixed.loops.front().accesses.front().guards_loop,
            "Load-Provenienz durch AND bis zum Guard wird nicht als mixed erhalten.");

    std::vector<std::uint8_t> unresolved_mixed_bytes(0x2Cu, 0u);
    write_u16(unresolved_mixed_bytes, 0x00u, 0xA002u); // BRA 0x08
    write_u16(unresolved_mixed_bytes, 0x02u, 0x0009u); // NOP
    write_u16(unresolved_mixed_bytes, 0x08u, 0xD107u); // MOV.L @(0x28,PC),R1
    write_u16(unresolved_mixed_bytes, 0x0Au, 0x6012u); // MOV.L @R1,R0
    write_u16(unresolved_mixed_bytes, 0x0Cu, 0xC901u); // AND #1,R0
    write_u16(unresolved_mixed_bytes, 0x0Eu, 0xA001u); // BRA 0x14
    write_u16(unresolved_mixed_bytes, 0x10u, 0x0009u); // NOP
    write_u16(unresolved_mixed_bytes, 0x14u, 0x2008u); // TST R0,R0
    write_u16(unresolved_mixed_bytes, 0x16u, 0x8905u); // BT 0x24
    write_u16(unresolved_mixed_bytes, 0x18u, 0x4210u); // DT R2
    write_u16(unresolved_mixed_bytes, 0x1Au, 0x8BF5u); // BF 0x08
    write_u16(unresolved_mixed_bytes, 0x1Cu, 0x000Bu); // RTS
    write_u16(unresolved_mixed_bytes, 0x1Eu, 0x0009u); // NOP
    write_u16(unresolved_mixed_bytes, 0x24u, 0x000Bu); // RTS
    write_u16(unresolved_mixed_bytes, 0x26u, 0x0009u); // NOP
    write_u32(unresolved_mixed_bytes, 0x28u, 0x0C003000u);
    const auto unresolved_mixed =
        audit_fixture("unresolved-transformed-guard", std::move(unresolved_mixed_bytes));
    require(unresolved_mixed.loops.size() == 1u &&
                unresolved_mixed.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                unresolved_mixed.loops.front().unresolved_guard_access &&
                !unresolved_mixed.loops.front().counter_instruction_addresses.empty() &&
                unresolved_mixed.loops.front().accesses.size() == 1u &&
                !unresolved_mixed.loops.front().accesses.front().guards_loop,
            "Ungeklaerte Guard-Herkunft aus dem Vorgaengerblock wird als Counterloop eingestuft.");

    std::vector<std::uint8_t> unknown_bytes(0x0Eu, 0u);
    write_u16(unknown_bytes, 0x00u, 0xA002u); // BRA 0x08
    write_u16(unknown_bytes, 0x02u, 0x0009u); // NOP
    write_u16(unknown_bytes, 0x08u, 0x0009u); // NOP
    write_u16(unknown_bytes, 0x0Au, 0xAFFDu); // BRA 0x08
    write_u16(unknown_bytes, 0x0Cu, 0x0009u); // NOP
    const auto unknown = audit_fixture("unknown-loop", std::move(unknown_bytes));
    require(unknown.loops.size() == 1u &&
                unknown.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown,
            "Schleife ohne belastbare Counter- oder Poll-Evidenz wird geraten statt unknown.");

    std::vector<std::uint8_t> irreducible_bytes(0x14u, 0u);
    write_u16(irreducible_bytes, 0x00u, 0x8902u); // BT 0x08
    write_u16(irreducible_bytes, 0x02u, 0xA005u); // BRA 0x10
    write_u16(irreducible_bytes, 0x04u, 0x0009u); // NOP
    write_u16(irreducible_bytes, 0x08u, 0xA002u); // BRA 0x10
    write_u16(irreducible_bytes, 0x0Au, 0x0009u); // NOP
    write_u16(irreducible_bytes, 0x10u, 0xAFFAu); // BRA 0x08
    write_u16(irreducible_bytes, 0x12u, 0x0009u); // NOP
    const auto irreducible =
        audit_fixture("non-dominating-backward-edge", std::move(irreducible_bytes));
    require(irreducible.loops.empty(),
            "Rueckwaertsziel ohne Dominanz wird faelschlich als natuerliche Backedge akzeptiert.");

    std::vector<std::uint8_t> contextual_bytes(0x10u, 0u);
    write_u16(contextual_bytes, 0x00u, 0xA002u); // BRA 0x08
    write_u16(contextual_bytes, 0x02u, 0x0009u); // NOP: delay slot and normal entry
    write_u16(contextual_bytes, 0x04u, 0x000Bu); // RTS
    write_u16(contextual_bytes, 0x06u, 0x0009u); // NOP
    write_u16(contextual_bytes, 0x08u, 0x4010u); // DT R0
    write_u16(contextual_bytes, 0x0Au, 0x8BFDu); // BF 0x08
    write_u16(contextual_bytes, 0x0Cu, 0x000Bu); // RTS
    write_u16(contextual_bytes, 0x0Eu, 0x0009u); // NOP
    katana::io::ExecutableImage contextual_image("normal-and-delay-slot-loop");
    contextual_image.add_segment({".text",
                                  0u,
                                  0u,
                                  contextual_bytes.size(),
                                  katana::io::SegmentKind::Code,
                                  {true, false, true},
                                  std::move(contextual_bytes)});
    contextual_image.add_entry_point(0u);
    contextual_image.add_entry_point(2u);
    const auto contextual_analysis = katana::analysis::analyze_control_flow(contextual_image);
    const auto overlapping_contexts =
        std::count_if(contextual_analysis.recursive.contextual_instructions.begin(),
                      contextual_analysis.recursive.contextual_instructions.end(),
                      [](const auto& context) { return context.line.address == 2u; });
    const auto contextual_audit =
        katana::analysis::audit_dreamcast_hardware(contextual_image, contextual_analysis);
    require(overlapping_contexts >= 2u && contextual_audit.loops.size() == 1u &&
                contextual_audit.loops.front().header_address == 0x08u &&
                contextual_audit.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Counter,
            "Normal-/Delay-Slot-Doppelrolle verliert den belegten Branch-Successor zur Loop.");

    std::vector<std::uint8_t> rootless_bytes(4u, 0u);
    write_u16(rootless_bytes, 0x00u, 0xAFFEu); // BRA 0x00
    write_u16(rootless_bytes, 0x02u, 0x0009u); // NOP
    katana::io::ExecutableImage rootless_image("rootless-scc");
    rootless_image.add_segment({".text",
                               0u,
                               0u,
                               rootless_bytes.size(),
                               katana::io::SegmentKind::Code,
                               {true, false, true},
                               std::move(rootless_bytes)});
    rootless_image.add_entry_point(0u);
    auto rootless_analysis = katana::analysis::analyze_control_flow(rootless_image);
    rootless_analysis.recursive.functions.clear();
    const auto rootless_audit =
        katana::analysis::audit_dreamcast_hardware(rootless_image, rootless_analysis);
    require(rootless_audit.loops.empty(),
            "Wurzellose SCC wird durch willkuerliche Adresswurzel als natuerliche Loop behauptet.");

    constexpr std::size_t scale_blocks = 4096u;
    std::vector<std::uint8_t> scale_bytes(scale_blocks * 4u, 0u);
    for (std::size_t block = 0u; block + 1u < scale_blocks; ++block) {
        write_u16(scale_bytes, block * 4u, 0xA000u); // BRA to the next block
        write_u16(scale_bytes, block * 4u + 2u, 0x0009u);
    }
    write_u16(scale_bytes, (scale_blocks - 1u) * 4u, 0xA800u); // BRA -4096 bytes
    write_u16(scale_bytes, (scale_blocks - 1u) * 4u + 2u, 0x0009u);
    const auto scale = audit_fixture("large-natural-loop-cfg", std::move(scale_bytes));
    require(scale.loops.size() == 1u && scale.loops.front().header_address == 0x3000u &&
                scale.loops.front().latch_address == 0x3FFCu &&
                scale.loops.front().block_addresses.size() == 1024u,
            "Skalierbarer Dominatorlauf verliert die grosse natuerliche Loopblockmenge.");

    const auto loop_json = katana::analysis::format_hardware_audit_json(mmio_poll);
    require(loop_json.find("\"classification\":\"mmio_poll\"") != std::string::npos &&
                loop_json.find("\"block_addresses\":[\"0x00000008\",\"0x00000010\"]") !=
                    std::string::npos &&
                loop_json.find("\"guards_loop\":true") != std::string::npos,
            "Maschinenlesbarer Loopvertrag verliert Klasse, Blockmenge oder Guard-Evidenz.");

    std::cout << "Allgemeines Dreamcast-Hardware-Audit erfolgreich.\n";
}
