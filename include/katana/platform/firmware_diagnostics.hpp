#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace katana::platform {

inline constexpr std::uint32_t firmware_diagnostic_schema_version = 1u;

enum class FirmwareImageKind : std::uint8_t { Bios, Flash };
enum class FirmwareRangeKind : std::uint8_t { Code, Data, Unknown };

struct FirmwareDiagnosticOptions {
    std::optional<std::string> expected_sha256;
    bool include_sensitive = false;
};

struct FirmwareRangeDiagnostic {
    std::size_t offset = 0u;
    std::size_t size = 0u;
    FirmwareRangeKind kind = FirmwareRangeKind::Unknown;
};

struct FlashLogicalBlockDiagnostic {
    std::uint16_t logical_id = 0u;
    std::size_t generations = 0u;
    std::size_t latest_physical_block = 0u;
    std::size_t valid_crc_generations = 0u;
};

struct FlashPartitionDiagnostic {
    std::uint8_t id = 0u;
    std::size_t offset = 0u;
    std::size_t size = 0u;
    bool block_allocated = false;
    bool header_valid = false;
    std::size_t allocated_records = 0u;
    std::size_t valid_crc_records = 0u;
    std::size_t invalid_crc_records = 0u;
    std::vector<FlashLogicalBlockDiagnostic> logical_blocks;
};

struct FirmwareDiagnosticReport {
    FirmwareImageKind kind = FirmwareImageKind::Bios;
    std::uint64_t size = 0u;
    std::uint64_t expected_size = 0u;
    bool size_valid = false;
    std::string sha256;
    std::optional<std::string> expected_sha256;
    bool hash_valid = true;
    bool read_only = true;
    bool sensitive_values_included = false;
    std::optional<std::string> sensitive_region_code;
    std::vector<FirmwareRangeDiagnostic> ranges;
    std::vector<FlashPartitionDiagnostic> partitions;

    [[nodiscard]] bool valid() const noexcept { return size_valid && hash_valid; }
};

[[nodiscard]] FirmwareDiagnosticReport inspect_firmware_file(
    const std::filesystem::path& path,
    FirmwareImageKind kind,
    const FirmwareDiagnosticOptions& options = {}
);
[[nodiscard]] std::string format_firmware_diagnostic_json(
    const FirmwareDiagnosticReport& report
);
[[nodiscard]] const char* firmware_image_kind_name(FirmwareImageKind kind) noexcept;
[[nodiscard]] const char* firmware_range_kind_name(FirmwareRangeKind kind) noexcept;

}
