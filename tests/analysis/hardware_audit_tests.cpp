#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/hardware_audit.hpp"

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
    require(json.find("\"schema\":\"katana.hardware-audit.v2\"") != std::string::npos &&
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

    std::cout << "Allgemeines Dreamcast-Hardware-Audit erfolgreich.\n";
}
