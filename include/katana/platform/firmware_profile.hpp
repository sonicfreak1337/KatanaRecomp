#pragma once

#include "katana/platform/dreamcast.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace katana::platform {

inline constexpr std::uint32_t alpha_firmware_contract_version = 1u;

enum class FirmwareProfileStatus { Available, ContractOnly, Unsupported };

struct AlphaFirmwareModeContract {
    FirmwareMode mode = FirmwareMode::DirectHomebrew;
    std::string_view id;
    FirmwareProfileStatus status = FirmwareProfileStatus::Unsupported;
    bool retail_boot = false;
    bool accepts_bios_source = false;
    bool accepts_flash_source = false;
    bool requires_bios_source = false;
    bool source_inputs_read_only = true;
    bool source_inputs_packaged = false;
    bool mutable_flash_uses_working_copy = false;
    std::string_view boot_state;
    std::string_view limitation;
    std::string_view test_requirement;
};

struct AlphaFirmwareInputPolicy {
    std::optional<std::filesystem::path> bios_source;
    std::optional<std::filesystem::path> flash_source;
    std::optional<std::filesystem::path> mutable_working_directory;
    std::optional<std::filesystem::path> port_output_directory;
};

[[nodiscard]] std::span<const AlphaFirmwareModeContract> alpha_firmware_contracts() noexcept;
[[nodiscard]] const AlphaFirmwareModeContract& alpha_firmware_contract(FirmwareMode mode) noexcept;
[[nodiscard]] const char* firmware_profile_status_name(FirmwareProfileStatus status) noexcept;

// Validates the complete request before a loader, memory mapper or CPU reset may run.
void require_alpha_firmware_profile(FirmwareMode mode, const AlphaFirmwareInputPolicy& inputs = {});

[[nodiscard]] std::string format_alpha_firmware_contract_json();

} // namespace katana::platform
