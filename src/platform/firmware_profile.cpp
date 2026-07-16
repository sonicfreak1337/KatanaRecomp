#include "katana/platform/firmware_profile.hpp"

#include "katana/io/json_report.hpp"

#include <array>
#include <cctype>
#include <cwctype>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace katana::platform {
namespace {

constexpr std::array kContracts{
    AlphaFirmwareModeContract{FirmwareMode::DirectHomebrew,
                              "direct",
                              FirmwareProfileStatus::Available,
                              true,
                              false,
                              false,
                              false,
                              true,
                              false,
                              false,
                              "validated executable image at the declared entry point",
                              "No BIOS ABI, firmware vectors or firmware MMIO initialization.",
                              "Synthetic GDI/direct-entry boot plus rejection of firmware inputs."},
    AlphaFirmwareModeContract{
        FirmwareMode::HleBiosAbi,
        "hle",
        FirmwareProfileStatus::ContractOnly,
        true,
        false,
        true,
        false,
        true,
        false,
        true,
        "direct retail entry with dynamically installed BIOS ABI vectors",
        "BIOS ABI services and dynamic vectors become executable in KR-4602.",
        "Synthetic ABI vector installation, known/unknown calls and ROM/RAM handoff."},
    AlphaFirmwareModeContract{
        FirmwareMode::LleFirmware,
        "lle",
        FirmwareProfileStatus::Unsupported,
        false,
        true,
        true,
        true,
        true,
        false,
        true,
        "firmware reset vector from a validated local image",
        "Optional LLE is not implemented or required for the Alpha profile.",
        "Pre-effect rejection; if enabled later, size, integrity, aliases and reset vector."}};

std::filesystem::path::string_type normalized_component(std::filesystem::path::string_type value) {
#ifdef _WIN32
    for (auto& character : value) {
        if constexpr (std::is_same_v<std::filesystem::path::value_type, wchar_t>) {
            character = static_cast<wchar_t>(std::towlower(character));
        } else {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
#endif
    return value;
}

bool path_contains(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const auto normalized_parent = parent.lexically_normal();
    const auto normalized_child = child.lexically_normal();
    auto parent_part = normalized_parent.begin();
    auto child_part = normalized_child.begin();
    for (; parent_part != normalized_parent.end(); ++parent_part, ++child_part) {
        if (child_part == normalized_child.end() ||
            normalized_component(parent_part->native()) !=
                normalized_component(child_part->native())) {
            return false;
        }
    }
    return true;
}

void require_external_to_port(const std::optional<std::filesystem::path>& port,
                              const std::optional<std::filesystem::path>& candidate,
                              const char* label) {
    if (port && candidate && path_contains(*port, *candidate)) {
        throw std::invalid_argument(std::string(label) +
                                    " muss ausserhalb des Port-Ausgabeordners liegen.");
    }
}

} // namespace

std::span<const AlphaFirmwareModeContract> alpha_firmware_contracts() noexcept {
    return kContracts;
}

const AlphaFirmwareModeContract& alpha_firmware_contract(const FirmwareMode mode) noexcept {
    for (const auto& contract : kContracts) {
        if (contract.mode == mode) return contract;
    }
    return kContracts.back();
}

const char* firmware_profile_status_name(const FirmwareProfileStatus status) noexcept {
    switch (status) {
    case FirmwareProfileStatus::Available:
        return "available";
    case FirmwareProfileStatus::ContractOnly:
        return "contract-only";
    case FirmwareProfileStatus::Unsupported:
        return "unsupported";
    }
    return "unsupported";
}

void require_alpha_firmware_profile(const FirmwareMode mode,
                                    const AlphaFirmwareInputPolicy& inputs) {
    require_external_to_port(inputs.port_output_directory, inputs.bios_source, "BIOS-Quelle");
    require_external_to_port(inputs.port_output_directory, inputs.flash_source, "Flash-Quelle");
    require_external_to_port(
        inputs.port_output_directory, inputs.mutable_working_directory, "Firmware-Arbeitskopie");

    const auto& contract = alpha_firmware_contract(mode);
    if (!contract.accepts_bios_source && inputs.bios_source) {
        throw std::invalid_argument("Firmwareprofil '" + std::string(contract.id) +
                                    "' akzeptiert keine BIOS-Quelle.");
    }
    if (!contract.accepts_flash_source && inputs.flash_source) {
        throw std::invalid_argument("Firmwareprofil '" + std::string(contract.id) +
                                    "' akzeptiert keine Flash-Quelle.");
    }
    if (contract.requires_bios_source && !inputs.bios_source) {
        throw std::invalid_argument("Firmwareprofil '" + std::string(contract.id) +
                                    "' braucht eine lokale BIOS-Quelle.");
    }
    if (inputs.flash_source && contract.mutable_flash_uses_working_copy &&
        !inputs.mutable_working_directory) {
        throw std::invalid_argument(
            "Eine lokale Flash-Quelle braucht eine getrennte kontrollierte Arbeitskopie.");
    }
    if (contract.status != FirmwareProfileStatus::Available) {
        throw std::invalid_argument("Firmwareprofil '" + std::string(contract.id) + "' ist " +
                                    firmware_profile_status_name(contract.status) + ": " +
                                    std::string(contract.limitation));
    }
}

std::string format_alpha_firmware_contract_json() {
    std::ostringstream output;
    output << "{\"schema\":\"katana-alpha-firmware\",\"version\":"
           << alpha_firmware_contract_version << ",\"modes\":[";
    for (std::size_t index = 0u; index < kContracts.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& contract = kContracts[index];
        output << "{\"id\":" << io::quote_json(contract.id)
               << ",\"status\":" << io::quote_json(firmware_profile_status_name(contract.status))
               << ",\"retail_boot\":" << (contract.retail_boot ? "true" : "false")
               << ",\"accepts_bios_source\":" << (contract.accepts_bios_source ? "true" : "false")
               << ",\"accepts_flash_source\":" << (contract.accepts_flash_source ? "true" : "false")
               << ",\"requires_bios_source\":" << (contract.requires_bios_source ? "true" : "false")
               << ",\"source_inputs_read_only\":"
               << (contract.source_inputs_read_only ? "true" : "false")
               << ",\"source_inputs_packaged\":"
               << (contract.source_inputs_packaged ? "true" : "false")
               << ",\"mutable_flash_uses_working_copy\":"
               << (contract.mutable_flash_uses_working_copy ? "true" : "false")
               << ",\"boot_state\":" << io::quote_json(contract.boot_state)
               << ",\"limitation\":" << io::quote_json(contract.limitation)
               << ",\"test_requirement\":" << io::quote_json(contract.test_requirement) << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace katana::platform
