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
    require(audit.memory_access_sites == 12u && audit.resolved_memory_access_sites == 12u &&
                audit.unresolved_memory_access_sites == 0u,
            "Statische Auditabdeckung zaehlt aufgeloeste Speicherstellen falsch.");
    require(audit.addresses.front().guest_address == 0xA05F6800u &&
                audit.addresses.front().runtime_support ==
                    katana::analysis::HardwareRuntimeSupport::Rejected &&
                audit.addresses.front().widths == std::vector<std::uint8_t>({1u, 4u}),
            "Gemischte Zugriffsbreiten behalten nicht die schwaechste Runtime-Capability.");

    const auto json = katana::analysis::format_hardware_audit_json(audit);
    require(json.find("\"schema\":\"katana.hardware-audit.v4\"") != std::string::npos &&
                json.find("\"scope\":\"executable_image\"") != std::string::npos &&
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

    std::vector<std::uint8_t> gbr_access_bytes(0x24u, 0u);
    write_u16(gbr_access_bytes, 0x00u, 0xD107u); // MOV.L @(0x20,PC),R1
    write_u16(gbr_access_bytes, 0x02u, 0x411Eu); // LDC R1,GBR
    write_u16(gbr_access_bytes, 0x04u, 0xE000u); // MOV #0,R0
    write_u16(gbr_access_bytes, 0x06u, 0xC001u); // MOV.B R0,@(1,GBR)
    write_u16(gbr_access_bytes, 0x08u, 0xC102u); // MOV.W R0,@(4,GBR)
    write_u16(gbr_access_bytes, 0x0Au, 0xC203u); // MOV.L R0,@(12,GBR)
    write_u16(gbr_access_bytes, 0x0Cu, 0xCC10u); // TST.B #16,@(R0,GBR)
    write_u16(gbr_access_bytes, 0x0Eu, 0xCD20u); // AND.B #32,@(R0,GBR)
    write_u16(gbr_access_bytes, 0x10u, 0xCE40u); // XOR.B #64,@(R0,GBR)
    write_u16(gbr_access_bytes, 0x12u, 0xCF80u); // OR.B #128,@(R0,GBR)
    write_u16(gbr_access_bytes, 0x14u, 0xC404u); // MOV.B @(4,GBR),R0
    write_u16(gbr_access_bytes, 0x16u, 0xC503u); // MOV.W @(6,GBR),R0
    write_u16(gbr_access_bytes, 0x18u, 0xC602u); // MOV.L @(8,GBR),R0
    write_u16(gbr_access_bytes, 0x1Au, 0x000Bu); // RTS
    write_u16(gbr_access_bytes, 0x1Cu, 0x0009u); // NOP
    write_u32(gbr_access_bytes, 0x20u, 0x00800000u);
    const auto gbr_accesses =
        audit_fixture("gbr-memory-families", std::move(gbr_access_bytes));
    const auto references_at = [](const katana::analysis::DreamcastHardwareAudit& source,
                                  const std::uint32_t instruction_address) {
        return static_cast<std::size_t>(std::count_if(
            source.references.begin(), source.references.end(), [&](const auto& reference) {
                return reference.instruction_address == instruction_address;
            }));
    };
    const auto accesses_of_kind = [](const katana::analysis::DreamcastHardwareAudit& source,
                                     const katana::analysis::HardwareAccessKind kind) {
        return static_cast<std::size_t>(std::count_if(
            source.references.begin(), source.references.end(),
            [kind](const auto& reference) { return reference.kind == kind; }));
    };
    require(gbr_accesses.memory_access_sites == 11u &&
                gbr_accesses.resolved_memory_access_sites == 11u &&
                gbr_accesses.unresolved_memory_access_sites == 0u &&
                gbr_accesses.references.size() == 13u &&
                accesses_of_kind(gbr_accesses, katana::analysis::HardwareAccessKind::Read) == 7u &&
                accesses_of_kind(gbr_accesses, katana::analysis::HardwareAccessKind::Write) == 6u &&
                references_at(gbr_accesses, 0x0Cu) == 1u &&
                references_at(gbr_accesses, 0x0Eu) == 2u &&
                references_at(gbr_accesses, 0x10u) == 2u &&
                references_at(gbr_accesses, 0x12u) == 2u,
            "GBR-MOV- oder Byte-Read-Modify-Write-Familien fehlen im Hardwareauditor.");
    require(std::all_of(gbr_accesses.references.begin(),
                        gbr_accesses.references.end(),
                        [](const auto& reference) {
                            return reference.region ==
                                   katana::analysis::DreamcastHardwareRegion::AicaRam;
                        }),
            "Konstantes GBR wird nicht auf die allgemeine Hardwareapertur fortgeschrieben.");

    std::vector<std::uint8_t> fmov_access_bytes(0x34u, 0u);
    write_u16(fmov_access_bytes, 0x00u, 0xD10Bu); // MOV.L @(0x30,PC),R1
    write_u16(fmov_access_bytes, 0x02u, 0xE000u); // MOV #0,R0
    write_u16(fmov_access_bytes, 0x04u, 0x6213u); // MOV R1,R2
    write_u16(fmov_access_bytes, 0x06u, 0x6313u); // MOV R1,R3
    write_u16(fmov_access_bytes, 0x08u, 0x6413u); // MOV R1,R4
    write_u16(fmov_access_bytes, 0x0Au, 0x6513u); // MOV R1,R5
    write_u16(fmov_access_bytes, 0x0Cu, 0x6613u); // MOV R1,R6
    write_u16(fmov_access_bytes, 0x0Eu, 0x6713u); // MOV R1,R7
    write_u16(fmov_access_bytes, 0x10u, 0xF028u); // FMOV.S @R2,FR0
    write_u16(fmov_access_bytes, 0x12u, 0xF039u); // FMOV.S @R3+,FR0
    write_u16(fmov_access_bytes, 0x14u, 0xF046u); // FMOV.S @(R0,R4),FR0
    write_u16(fmov_access_bytes, 0x16u, 0xF40Au); // FMOV.S FR0,@R4
    write_u16(fmov_access_bytes, 0x18u, 0xF50Bu); // FMOV.S FR0,@-R5
    write_u16(fmov_access_bytes, 0x1Au, 0xF607u); // FMOV.S FR0,@(R0,R6)
    write_u16(fmov_access_bytes, 0x1Cu, 0x471Bu); // TAS.B @R7
    write_u16(fmov_access_bytes, 0x1Eu, 0x000Bu); // RTS
    write_u16(fmov_access_bytes, 0x20u, 0x0009u); // NOP
    write_u32(fmov_access_bytes, 0x30u, 0x00801000u);
    const auto fmov_accesses =
        audit_fixture("fmov-and-tas-memory-families", std::move(fmov_access_bytes));
    require(fmov_accesses.memory_access_sites == 8u &&
                fmov_accesses.resolved_memory_access_sites == 8u &&
                fmov_accesses.unresolved_memory_access_sites == 0u &&
                fmov_accesses.references.size() == 14u &&
                accesses_of_kind(fmov_accesses, katana::analysis::HardwareAccessKind::Read) == 7u &&
                accesses_of_kind(fmov_accesses, katana::analysis::HardwareAccessKind::Write) == 7u &&
                references_at(fmov_accesses, 0x1Cu) == 2u,
            "FMOV-Buszugriffe oder TAS.B-Read-Modify-Write werden nicht vollstaendig erfasst.");
    require(std::count_if(fmov_accesses.references.begin(),
                          fmov_accesses.references.end(),
                          [](const auto& reference) {
                              return reference.instruction_address == 0x18u &&
                                     (reference.guest_address == 0x00800FF8u ||
                                      reference.guest_address == 0x00800FFCu) &&
                                     reference.width == 4u;
                          }) == 2,
            "FMOV-Predecrement bildet die beiden moeglichen 32-Bit-Busadressen falsch ab.");

    std::vector<std::uint8_t> fcmp_poll_bytes(0x18u, 0u);
    write_u16(fcmp_poll_bytes, 0x00u, 0xD104u); // MOV.L @(0x14,PC),R1
    write_u16(fcmp_poll_bytes, 0x02u, 0xF018u); // FMOV.S @R1,FR0
    write_u16(fcmp_poll_bytes, 0x04u, 0xF18Du); // FLDI0 FR1
    write_u16(fcmp_poll_bytes, 0x06u, 0xF014u); // FCMP/EQ FR1,FR0
    write_u16(fcmp_poll_bytes, 0x08u, 0x8BFAu); // BF 0x00
    write_u16(fcmp_poll_bytes, 0x0Au, 0x000Bu); // RTS
    write_u16(fcmp_poll_bytes, 0x0Cu, 0x0009u); // NOP
    write_u32(fcmp_poll_bytes, 0x14u, 0xA05F8000u);
    const auto fcmp_poll = audit_fixture("fmov-fcmp-poll", std::move(fcmp_poll_bytes));
    require(fcmp_poll.loops.size() == 1u &&
                fcmp_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                fcmp_poll.loops.front().unresolved_guard_access &&
                fcmp_poll.loops.front().unresolved_guard_read_instruction_addresses ==
                    std::vector<std::uint32_t>({0x00u, 0x02u}) &&
                std::none_of(fcmp_poll.loops.front().accesses.begin(),
                             fcmp_poll.loops.front().accesses.end(),
                             [](const auto& access) { return access.guards_loop; }) &&
                fcmp_poll.unresolved_poll_guard_loops == 1u,
            "FMOV-/FCMP-Polling wird ohne PR/SZ/FR-/FPUL-Provenienz unsicher freigegeben.");

    std::vector<std::uint8_t> dynamic_fcmp_poll_bytes(0x0Cu, 0u);
    write_u16(dynamic_fcmp_poll_bytes, 0x00u, 0xF018u); // FMOV.S @R1,FR0
    write_u16(dynamic_fcmp_poll_bytes, 0x02u, 0xF18Du); // FLDI0 FR1
    write_u16(dynamic_fcmp_poll_bytes, 0x04u, 0xF014u); // FCMP/EQ FR1,FR0
    write_u16(dynamic_fcmp_poll_bytes, 0x06u, 0x8BFBu); // BF 0x00
    write_u16(dynamic_fcmp_poll_bytes, 0x08u, 0x000Bu); // RTS
    write_u16(dynamic_fcmp_poll_bytes, 0x0Au, 0x0009u); // NOP
    const auto dynamic_fcmp_poll =
        audit_fixture("dynamic-fmov-fcmp-poll", std::move(dynamic_fcmp_poll_bytes));
    require(dynamic_fcmp_poll.loops.size() == 1u &&
                dynamic_fcmp_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                dynamic_fcmp_poll.loops.front().unresolved_guard_read_instruction_addresses ==
                    std::vector<std::uint32_t>({0x00u}) &&
                dynamic_fcmp_poll.unresolved_poll_guard_loops == 1u,
            "Dynamisches FMOV-/FCMP-Polling wird im Strict-Modus faelschlich freigegeben.");

    std::vector<std::uint8_t> pc_relative_bytes(0x10u, 0u);
    write_u16(pc_relative_bytes, 0x00u, 0x9002u); // MOV.W @(0x08,PC),R0
    write_u16(pc_relative_bytes, 0x02u, 0xD102u); // MOV.L @(0x0C,PC),R1
    write_u16(pc_relative_bytes, 0x04u, 0x000Bu); // RTS
    write_u16(pc_relative_bytes, 0x06u, 0x0009u); // NOP
    write_u16(pc_relative_bytes, 0x08u, 0x1234u);
    write_u32(pc_relative_bytes, 0x0Cu, 0x89ABCDEFu);
    const auto pc_relative_accesses =
        audit_fixture("pc-relative-memory-families", std::move(pc_relative_bytes));
    require(pc_relative_accesses.memory_access_sites == 2u &&
                pc_relative_accesses.resolved_memory_access_sites == 2u &&
                pc_relative_accesses.unresolved_memory_access_sites == 0u,
            "PC-relative MOV.W-/MOV.L-Reads fehlen gegenueber dem zentralen Memory-Effect-Vertrag.");

    std::vector<std::uint8_t> special_memory_bytes(0x20u, 0u);
    write_u16(special_memory_bytes, 0x00u, 0xD105u); // MOV.L @(0x18,PC),R1
    write_u16(special_memory_bytes, 0x02u, 0x4103u); // STC.L SR,@-R1
    write_u16(special_memory_bytes, 0x04u, 0xD205u); // MOV.L @(0x1C,PC),R2
    write_u16(special_memory_bytes, 0x06u, 0x4207u); // LDC.L @R2+,SR
    write_u16(special_memory_bytes, 0x08u, 0x000Bu); // RTS
    write_u16(special_memory_bytes, 0x0Au, 0x0009u); // NOP
    write_u32(special_memory_bytes, 0x18u, 0xA05F8004u);
    write_u32(special_memory_bytes, 0x1Cu, 0xA05F8000u);
    const auto special_memory_accesses =
        audit_fixture("special-register-memory-families", std::move(special_memory_bytes));
    require(special_memory_accesses.memory_access_sites == 4u &&
                special_memory_accesses.resolved_memory_access_sites == 4u &&
                special_memory_accesses.references.size() == 2u &&
                references_at(special_memory_accesses, 0x02u) == 1u &&
                references_at(special_memory_accesses, 0x06u) == 1u &&
                accesses_of_kind(special_memory_accesses,
                                 katana::analysis::HardwareAccessKind::Read) == 1u &&
                accesses_of_kind(special_memory_accesses,
                                 katana::analysis::HardwareAccessKind::Write) == 1u,
            "STC.L-Predecrement oder LDC.L-Postincrement fehlt im Hardwareauditor.");

    std::vector<std::uint8_t> mac_memory_bytes(0x40u, 0u);
    write_u16(mac_memory_bytes, 0x00u, 0xD10Bu); // MOV.L @(0x30,PC),R1
    write_u16(mac_memory_bytes, 0x02u, 0xD20Cu); // MOV.L @(0x34,PC),R2
    write_u16(mac_memory_bytes, 0x04u, 0x412Fu); // MAC.W @R2+,@R1+
    write_u16(mac_memory_bytes, 0x06u, 0xD30Cu); // MOV.L @(0x38,PC),R3
    write_u16(mac_memory_bytes, 0x08u, 0xD40Cu); // MOV.L @(0x3C,PC),R4
    write_u16(mac_memory_bytes, 0x0Au, 0x034Fu); // MAC.L @R4+,@R3+
    write_u16(mac_memory_bytes, 0x0Cu, 0x000Bu); // RTS
    write_u16(mac_memory_bytes, 0x0Eu, 0x0009u); // NOP
    write_u32(mac_memory_bytes, 0x30u, 0xA05F8000u);
    write_u32(mac_memory_bytes, 0x34u, 0xA05F8004u);
    write_u32(mac_memory_bytes, 0x38u, 0xA05F8010u);
    write_u32(mac_memory_bytes, 0x3Cu, 0xA05F8014u);
    const auto mac_memory_accesses =
        audit_fixture("multiply-accumulate-memory-families", std::move(mac_memory_bytes));
    require(mac_memory_accesses.memory_access_sites == 6u &&
                mac_memory_accesses.resolved_memory_access_sites == 6u &&
                mac_memory_accesses.references.size() == 4u &&
                references_at(mac_memory_accesses, 0x04u) == 2u &&
                references_at(mac_memory_accesses, 0x0Au) == 2u &&
                std::count_if(mac_memory_accesses.references.begin(),
                              mac_memory_accesses.references.end(),
                              [](const auto& reference) { return reference.width == 2u; }) == 2 &&
                std::count_if(mac_memory_accesses.references.begin(),
                              mac_memory_accesses.references.end(),
                              [](const auto& reference) { return reference.width == 4u; }) == 2,
            "MAC.W-/MAC.L-Doppelreads fehlen gegenueber dem zentralen Memory-Effect-Vertrag.");

    std::vector<std::uint8_t> same_mac_register_bytes(0x14u, 0u);
    write_u16(same_mac_register_bytes, 0x00u, 0xD103u); // MOV.L @(0x10,PC),R1
    write_u16(same_mac_register_bytes, 0x02u, 0x411Fu); // MAC.W @R1+,@R1+
    write_u16(same_mac_register_bytes, 0x04u, 0x000Bu); // RTS
    write_u16(same_mac_register_bytes, 0x06u, 0x0009u); // NOP
    write_u32(same_mac_register_bytes, 0x10u, 0xA05F8000u);
    const auto same_mac_register =
        audit_fixture("same-register-multiply-accumulate", std::move(same_mac_register_bytes));
    require(same_mac_register.references.size() == 2u &&
                references_at(same_mac_register, 0x02u) == 2u &&
                same_mac_register.references[0u].guest_address == 0xA05F8000u &&
                same_mac_register.references[1u].guest_address == 0xA05F8002u,
            "MAC mit identischen Quell-/Zielregistern liest den zweiten Operanden nach dem "
            "ersten Postincrement.");

    std::vector<std::uint8_t> partial_mac_bytes(0x14u, 0u);
    write_u16(partial_mac_bytes, 0x00u, 0xD103u); // MOV.L @(0x10,PC),R1
    write_u16(partial_mac_bytes, 0x02u, 0x412Fu); // MAC.W @R2+,@R1+
    write_u16(partial_mac_bytes, 0x04u, 0x000Bu); // RTS
    write_u16(partial_mac_bytes, 0x06u, 0x0009u); // NOP
    write_u32(partial_mac_bytes, 0x10u, 0xA05F8000u);
    const auto partial_mac =
        audit_fixture("partial-multiply-accumulate", std::move(partial_mac_bytes));
    require(partial_mac.memory_access_sites == 2u &&
                partial_mac.resolved_memory_access_sites == 1u &&
                partial_mac.unresolved_memory_access_sites == 1u &&
                partial_mac.references.size() == 1u &&
                partial_mac.references.front().instruction_address == 0x02u &&
                partial_mac.references.front().guest_address == 0xA05F8000u,
            "MAC mit nur einer bekannten Basis verwirft den unabhaengig aufgeloesten Zugriff.");

    std::vector<std::uint8_t> wrapping_predecrement_bytes(0x08u, 0u);
    write_u16(wrapping_predecrement_bytes, 0x00u, 0xE100u); // MOV #0,R1
    write_u16(wrapping_predecrement_bytes, 0x02u, 0x4103u); // STC.L SR,@-R1
    write_u16(wrapping_predecrement_bytes, 0x04u, 0x000Bu); // RTS
    write_u16(wrapping_predecrement_bytes, 0x06u, 0x0009u); // NOP
    const auto wrapping_predecrement =
        audit_fixture("wrapping-predecrement", std::move(wrapping_predecrement_bytes));
    require(wrapping_predecrement.memory_access_sites == 1u &&
                wrapping_predecrement.resolved_memory_access_sites == 1u &&
                wrapping_predecrement.references.size() == 1u &&
                wrapping_predecrement.references.front().guest_address == 0xFFFFFFFCu &&
                wrapping_predecrement.references.front().region ==
                    katana::analysis::DreamcastHardwareRegion::Sh4P4,
            "SH-4-Predecrement verliert die 32-Bit-Wraparound-Adresse.");

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
                ram_poll.loops.front().accesses.size() == 2u &&
                ram_poll.loops.front().accesses.back().linear_memory &&
                ram_poll.loops.front().accesses.back().canonical_address == 0x0C001000u &&
                ram_poll.loops.front().accesses.back().guards_loop,
            "Rueckwaerts angeordnete RAM-Pollbackedge oder Area-3-Spiegelkanonisierung fehlt.");

    const auto on_chip_ram_poll =
        audit_fixture("on-chip-ram-poll", backward_poll_fixture(0x7E001000u));
    require(on_chip_ram_poll.loops.size() == 1u &&
                on_chip_ram_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::MmioPoll &&
                on_chip_ram_poll.loops.front().accesses.size() == 2u &&
                on_chip_ram_poll.loops.front().accesses.back().region ==
                    katana::analysis::DreamcastHardwareRegion::Sh4OnChipRam &&
                !on_chip_ram_poll.loops.front().accesses.back().linear_memory &&
                on_chip_ram_poll.loops.front().accesses.back().guards_loop,
            "SH-4-On-Chip-RAM wird faelschlich als linearer Hauptspeicher-Poll klassifiziert.");

    const auto mmio_poll =
        audit_fixture("backward-layout-mmio-poll", backward_poll_fixture(0xA05F8000u));
    require(mmio_poll.loops.size() == 1u &&
                mmio_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::MmioPoll &&
                mmio_poll.loops.front().accesses.size() == 2u &&
                mmio_poll.loops.front().accesses.back().region ==
                    katana::analysis::DreamcastHardwareRegion::Pvr &&
                !mmio_poll.loops.front().accesses.back().linear_memory &&
                mmio_poll.loops.front().accesses.back().guards_loop,
            "MMIO-Polling wird nicht von linearem RAM-Polling getrennt.");

    std::vector<std::uint8_t> nop_guard_bytes(0x14u, 0u);
    write_u16(nop_guard_bytes, 0x00u, 0xD103u); // MOV.L @(0x10,PC),R1
    write_u16(nop_guard_bytes, 0x02u, 0x6012u); // MOV.L @R1,R0
    write_u16(nop_guard_bytes, 0x04u, 0x2008u); // TST R0,R0
    write_u16(nop_guard_bytes, 0x06u, 0x0009u); // NOP
    write_u16(nop_guard_bytes, 0x08u, 0x8BFAu); // BF 0x00
    write_u16(nop_guard_bytes, 0x0Au, 0x000Bu); // RTS
    write_u16(nop_guard_bytes, 0x0Cu, 0x0009u); // NOP
    write_u32(nop_guard_bytes, 0x10u, 0x0C004000u);
    const auto nop_guard = audit_fixture("nop-separated-poll-guard", std::move(nop_guard_bytes));
    require(nop_guard.loops.size() == 1u &&
                nop_guard.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::RamPoll &&
                !nop_guard.loops.front().unresolved_guard_access &&
                nop_guard.loops.front().accesses.size() == 2u &&
                nop_guard.loops.front().accesses.back().guards_loop,
            "Harmloses NOP zwischen T-Schreiber und BT/BF zerstoert die Guard-Provenienz.");

    std::vector<std::uint8_t> dynamic_guard_bytes(0x0Au, 0u);
    write_u16(dynamic_guard_bytes, 0x00u, 0x6012u); // MOV.L @R1,R0
    write_u16(dynamic_guard_bytes, 0x02u, 0x2008u); // TST R0,R0
    write_u16(dynamic_guard_bytes, 0x04u, 0x8BFCu); // BF 0x00
    write_u16(dynamic_guard_bytes, 0x06u, 0x000Bu); // RTS
    write_u16(dynamic_guard_bytes, 0x08u, 0x0009u); // NOP
    const auto dynamic_guard =
        audit_fixture("dynamic-address-poll-guard", std::move(dynamic_guard_bytes));
    require(dynamic_guard.loops.size() == 1u &&
                dynamic_guard.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                dynamic_guard.loops.front().unresolved_guard_access &&
                dynamic_guard.loops.front().unresolved_guard_read_instruction_addresses ==
                    std::vector<std::uint32_t>({0x00u}) &&
                dynamic_guard.loops.front().accesses.empty() &&
                dynamic_guard.unresolved_poll_guard_loops == 1u &&
                katana::analysis::is_unresolved_poll_guard_loop(dynamic_guard.loops.front()),
            "Dynamischer syntaktischer Read verliert seine Poll-/Guard-Provenienz im Strict-Modus.");
    const auto dynamic_guard_text =
        katana::analysis::format_hardware_audit_text(dynamic_guard);
    const auto dynamic_guard_json =
        katana::analysis::format_hardware_audit_json(dynamic_guard);
    require(dynamic_guard_text.find("unresolved_guard_read_sites=0x00000000") !=
                    std::string::npos &&
                dynamic_guard_json.find(
                    "\"unresolved_guard_read_instruction_addresses\":[\"0x00000000\"]") !=
                    std::string::npos,
            "Text-/JSON-Audit verliert die syntaktische unresolved-Read-Provenienz.");

    std::vector<std::uint8_t> overwritten_t_bytes(0x18u, 0u);
    write_u16(overwritten_t_bytes, 0x00u, 0xD104u); // MOV.L @(0x14,PC),R1
    write_u16(overwritten_t_bytes, 0x02u, 0x6012u); // MOV.L @R1,R0
    write_u16(overwritten_t_bytes, 0x04u, 0x2008u); // TST R0,R0
    write_u16(overwritten_t_bytes, 0x06u, 0x0018u); // SETT
    write_u16(overwritten_t_bytes, 0x08u, 0x0009u); // NOP
    write_u16(overwritten_t_bytes, 0x0Au, 0x8BF9u); // BF 0x00
    write_u16(overwritten_t_bytes, 0x0Cu, 0x000Bu); // RTS
    write_u16(overwritten_t_bytes, 0x0Eu, 0x0009u); // NOP
    write_u32(overwritten_t_bytes, 0x14u, 0x0C005000u);
    const auto overwritten_t =
        audit_fixture("intervening-t-writer", std::move(overwritten_t_bytes));
    require(overwritten_t.loops.size() == 1u &&
                overwritten_t.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                overwritten_t.loops.front().unresolved_guard_access &&
                overwritten_t.loops.front().unresolved_guard_read_instruction_addresses.empty() &&
                overwritten_t.loops.front().accesses.size() == 2u &&
                !overwritten_t.loops.front().accesses.back().guards_loop &&
                overwritten_t.unresolved_poll_guard_loops == 0u &&
                !katana::analysis::is_unresolved_poll_guard_loop(overwritten_t.loops.front()),
            "Intervenierender echter T-Schreiber wird faelschlich zum aelteren Poll durchgereicht.");

    std::vector<std::uint8_t> sr_guard_bytes(0x14u, 0u);
    write_u16(sr_guard_bytes, 0x00u, 0xD103u); // MOV.L @(0x10,PC),R1
    write_u16(sr_guard_bytes, 0x02u, 0x4107u); // LDC.L @R1+,SR
    write_u16(sr_guard_bytes, 0x04u, 0x8BFCu); // BF 0x00
    write_u16(sr_guard_bytes, 0x06u, 0x000Bu); // RTS
    write_u16(sr_guard_bytes, 0x08u, 0x0009u); // NOP
    write_u32(sr_guard_bytes, 0x10u, 0xA05F8000u);
    const auto sr_guard = audit_fixture("ldc-sr-memory-guard", std::move(sr_guard_bytes));
    require(sr_guard.loops.size() == 1u &&
                sr_guard.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::MmioPoll &&
                !sr_guard.loops.front().unresolved_guard_access &&
                sr_guard.loops.front().accesses.size() == 2u &&
                sr_guard.loops.front().accesses.back().instruction_address == 0x02u &&
                sr_guard.loops.front().accesses.back().kind ==
                    katana::analysis::HardwareAccessKind::Read &&
                sr_guard.loops.front().accesses.back().guards_loop,
            "LDC.L @Rm+,SR wird nicht als direkter speicherabhaengiger T-Writer erkannt.");

    std::vector<std::uint8_t> tst_byte_guard_bytes(0x1Cu, 0u);
    write_u16(tst_byte_guard_bytes, 0x00u, 0xD105u); // MOV.L @(0x18,PC),R1
    write_u16(tst_byte_guard_bytes, 0x02u, 0x411Eu); // LDC R1,GBR
    write_u16(tst_byte_guard_bytes, 0x04u, 0xE000u); // MOV #0,R0
    write_u16(tst_byte_guard_bytes, 0x06u, 0xCC01u); // TST.B #1,@(R0,GBR)
    write_u16(tst_byte_guard_bytes, 0x08u, 0x0009u); // NOP
    write_u16(tst_byte_guard_bytes, 0x0Au, 0x89F9u); // BT 0x00
    write_u16(tst_byte_guard_bytes, 0x0Cu, 0x000Bu); // RTS
    write_u16(tst_byte_guard_bytes, 0x0Eu, 0x0009u); // NOP
    write_u32(tst_byte_guard_bytes, 0x18u, 0x00802000u);
    const auto tst_byte_guard =
        audit_fixture("tst-byte-direct-memory-guard", std::move(tst_byte_guard_bytes));
    require(tst_byte_guard.loops.size() == 1u &&
                tst_byte_guard.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::RamPoll &&
                !tst_byte_guard.loops.front().unresolved_guard_access &&
                tst_byte_guard.loops.front().accesses.size() == 2u &&
                tst_byte_guard.loops.front().accesses.back().kind ==
                    katana::analysis::HardwareAccessKind::Read &&
                tst_byte_guard.loops.front().accesses.back().guards_loop,
            "Direkter TST.B-Speicherwächter wird nicht als Pollquelle erkannt.");

    std::vector<std::uint8_t> tas_guard_bytes(0x14u, 0u);
    write_u16(tas_guard_bytes, 0x00u, 0xD703u); // MOV.L @(0x10,PC),R7
    write_u16(tas_guard_bytes, 0x02u, 0x471Bu); // TAS.B @R7
    write_u16(tas_guard_bytes, 0x04u, 0x0009u); // NOP
    write_u16(tas_guard_bytes, 0x06u, 0x89FBu); // BT 0x00
    write_u16(tas_guard_bytes, 0x08u, 0x000Bu); // RTS
    write_u16(tas_guard_bytes, 0x0Au, 0x0009u); // NOP
    write_u32(tas_guard_bytes, 0x10u, 0xA05F8000u);
    const auto tas_guard = audit_fixture("tas-direct-memory-guard", std::move(tas_guard_bytes));
    require(tas_guard.loops.size() == 1u &&
                tas_guard.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::MmioPoll &&
                !tas_guard.loops.front().unresolved_guard_access &&
                tas_guard.loops.front().accesses.size() == 3u &&
                tas_guard.loops.front().accesses[1u].kind ==
                    katana::analysis::HardwareAccessKind::Read &&
                tas_guard.loops.front().accesses[1u].guards_loop &&
                tas_guard.loops.front().accesses.back().kind ==
                    katana::analysis::HardwareAccessKind::Write &&
                !tas_guard.loops.front().accesses.back().guards_loop,
            "TAS.B verliert Read-Modify-Write- oder direkte Guard-Evidenz.");

    const auto unmapped_poll =
        audit_fixture("unmapped-p4-poll", backward_poll_fixture(0xFF123456u));
    require(unmapped_poll.loops.size() == 1u &&
                unmapped_poll.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                unmapped_poll.loops.front().accesses.size() == 2u &&
                unmapped_poll.loops.front().accesses.back().region ==
                    katana::analysis::DreamcastHardwareRegion::Sh4P4 &&
                !unmapped_poll.loops.front().accesses.back().aperture_mapped &&
                unmapped_poll.loops.front().accesses.back().runtime_support ==
                    katana::analysis::HardwareRuntimeSupport::Unmapped &&
                unmapped_poll.loops.front().accesses.back().guards_loop,
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
                mixed.loops.front().accesses.size() == 2u &&
                mixed.loops.front().accesses.back().guards_loop,
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
    const auto predecessor_mixed =
        audit_fixture("unique-predecessor-transformed-guard", std::move(unresolved_mixed_bytes));
    require(predecessor_mixed.loops.size() == 1u &&
                predecessor_mixed.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Mixed &&
                !predecessor_mixed.loops.front().unresolved_guard_access &&
                !predecessor_mixed.loops.front().counter_instruction_addresses.empty() &&
                predecessor_mixed.loops.front().accesses.size() == 2u &&
                predecessor_mixed.loops.front().accesses.back().guards_loop,
            "Eindeutiger Vorgaengerblock verliert die transformierte Load-Guard-Provenienz.");

    std::vector<std::uint8_t> merged_guard_bytes(0x34u, 0u);
    write_u16(merged_guard_bytes, 0x00u, 0xA002u); // BRA 0x08
    write_u16(merged_guard_bytes, 0x02u, 0x0009u); // NOP
    write_u16(merged_guard_bytes, 0x08u, 0x8904u); // BT 0x14
    write_u16(merged_guard_bytes, 0x0Au, 0xD109u); // MOV.L @(0x30,PC),R1
    write_u16(merged_guard_bytes, 0x0Cu, 0x6012u); // MOV.L @R1,R0
    write_u16(merged_guard_bytes, 0x0Eu, 0xA003u); // BRA 0x18
    write_u16(merged_guard_bytes, 0x10u, 0x0009u); // NOP
    write_u16(merged_guard_bytes, 0x14u, 0xE000u); // MOV #0,R0
    write_u16(merged_guard_bytes, 0x16u, 0x0009u); // NOP
    write_u16(merged_guard_bytes, 0x18u, 0x2008u); // TST R0,R0
    write_u16(merged_guard_bytes, 0x1Au, 0x8905u); // BT 0x28
    write_u16(merged_guard_bytes, 0x1Cu, 0x4210u); // DT R2
    write_u16(merged_guard_bytes, 0x1Eu, 0x8BF3u); // BF 0x08
    write_u16(merged_guard_bytes, 0x20u, 0x000Bu); // RTS
    write_u16(merged_guard_bytes, 0x22u, 0x0009u); // NOP
    write_u16(merged_guard_bytes, 0x28u, 0x000Bu); // RTS
    write_u16(merged_guard_bytes, 0x2Au, 0x0009u); // NOP
    write_u32(merged_guard_bytes, 0x30u, 0x0C006000u);
    const auto merged_guard =
        audit_fixture("merged-predecessor-guard", std::move(merged_guard_bytes));
    require(merged_guard.loops.size() == 1u &&
                merged_guard.loops.front().classification ==
                    katana::analysis::HardwareLoopClassification::Unknown &&
                merged_guard.loops.front().unresolved_guard_access &&
                !merged_guard.loops.front().counter_instruction_addresses.empty() &&
                merged_guard.loops.front().accesses.size() == 2u &&
                !merged_guard.loops.front().accesses.back().guards_loop,
            "Mehrdeutiger Merge wird faelschlich als eindeutige Poll-Provenienz behauptet.");

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
