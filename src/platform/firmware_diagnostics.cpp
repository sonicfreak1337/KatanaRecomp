#include "katana/platform/firmware_diagnostics.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace katana::platform {
namespace {

struct FlashPartitionLayout {
    std::uint8_t id;
    std::size_t offset;
    std::size_t size;
    bool block_allocated;
};

constexpr std::array<FlashPartitionLayout, 5u> flash_layout{{{0u, 0x1A000u, 0x02000u, false},
                                                             {1u, 0x18000u, 0x02000u, false},
                                                             {2u, 0x1C000u, 0x04000u, true},
                                                             {3u, 0x10000u, 0x08000u, true},
                                                             {4u, 0x00000u, 0x10000u, true}}};
constexpr std::size_t flash_record_size = 64u;
constexpr std::size_t flash_crc_offset = 62u;
constexpr std::string_view flash_magic = "KATANA_FLASH____";

bool sha256_text(const std::string_view value) noexcept {
    return value.size() == 64u &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Firmwarediagnose konnte die Eingabe nicht oeffnen.");
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("Firmwarediagnose konnte die Groesse nicht lesen.");
    const auto unsigned_size = static_cast<std::uintmax_t>(end);
    if (unsigned_size > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("Firmwareeingabe ist fuer den Host zu gross.");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(unsigned_size));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) throw std::runtime_error("Firmwarediagnose konnte die Eingabe nicht lesen.");
    return bytes;
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u);
}

std::uint16_t flash_crc(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    std::uint32_t crc = 0xFFFFu;
    for (std::size_t index = 0u; index < flash_crc_offset; ++index) {
        crc ^= static_cast<std::uint32_t>(bytes[offset + index]) << 8u;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u ? (crc << 1u) ^ 0x1021u : crc << 1u;
            crc &= 0xFFFFu;
        }
    }
    return static_cast<std::uint16_t>((~crc) & 0xFFFFu);
}

bool bitmap_allocated(const std::vector<std::uint8_t>& bytes,
                      const std::size_t bitmap_offset,
                      const std::size_t index) {
    return (bytes[bitmap_offset + index / 8u] & (0x80u >> (index % 8u))) == 0u;
}

FlashPartitionDiagnostic inspect_partition(const std::vector<std::uint8_t>& bytes,
                                           const FlashPartitionLayout& layout) {
    FlashPartitionDiagnostic result;
    result.id = layout.id;
    result.offset = layout.offset;
    result.size = layout.size;
    result.block_allocated = layout.block_allocated;
    if (!layout.block_allocated) return result;

    result.header_valid = std::equal(flash_magic.begin(),
                                     flash_magic.end(),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(layout.offset)) &&
                          read_u16(bytes, layout.offset + flash_magic.size()) == layout.id;

    const auto record_bits = layout.size / flash_record_size;
    const auto rounded_bits = (record_bits + 511u) & ~std::size_t{511u};
    const auto bitmap_size = rounded_bits / 8u;
    const auto bitmap_offset = layout.offset + layout.size - bitmap_size;
    const auto maximum_records =
        std::min(record_bits, (layout.size - flash_record_size - bitmap_size) / flash_record_size);

    struct Aggregate {
        std::size_t generations = 0u;
        std::size_t latest = 0u;
        std::size_t valid = 0u;
    };
    std::map<std::uint16_t, Aggregate> logical;
    for (std::size_t record = 0u; record < maximum_records; ++record) {
        if (!bitmap_allocated(bytes, bitmap_offset, record)) break;
        const auto offset = layout.offset + (record + 1u) * flash_record_size;
        const auto logical_id = read_u16(bytes, offset);
        const bool crc_valid =
            flash_crc(bytes, offset) == read_u16(bytes, offset + flash_crc_offset);
        ++result.allocated_records;
        crc_valid ? ++result.valid_crc_records : ++result.invalid_crc_records;
        auto& aggregate = logical[logical_id];
        ++aggregate.generations;
        aggregate.latest = record + 1u;
        if (crc_valid) ++aggregate.valid;
    }
    result.logical_blocks.reserve(logical.size());
    for (const auto& [logical_id, aggregate] : logical) {
        result.logical_blocks.push_back(
            {logical_id, aggregate.generations, aggregate.latest, aggregate.valid});
    }
    return result;
}

std::optional<std::string> region_code(const std::vector<std::uint8_t>& bytes,
                                       const std::size_t offset) {
    std::string result;
    result.reserve(5u);
    for (std::size_t index = 0u; index < 5u; ++index) {
        const auto value = bytes[offset + index];
        if (value < 0x20u || value > 0x7Eu) return std::nullopt;
        result.push_back(static_cast<char>(value));
    }
    return result;
}

} // namespace

FirmwareDiagnosticReport inspect_firmware_file(const std::filesystem::path& path,
                                               const FirmwareImageKind kind,
                                               const FirmwareDiagnosticOptions& options) {
    if (path.empty()) throw std::invalid_argument("Firmwarediagnose braucht einen Eingabepfad.");
    if (options.expected_sha256.has_value() && !sha256_text(*options.expected_sha256)) {
        throw std::invalid_argument("Erwarteter Firmware-SHA-256 ist ungueltig.");
    }
    const auto bytes = read_file(path);
    FirmwareDiagnosticReport report;
    report.kind = kind;
    report.size = bytes.size();
    report.expected_size = kind == FirmwareImageKind::Bios ? katana::runtime::dreamcast_bios_size
                                                           : katana::runtime::dreamcast_flash_size;
    report.size_valid = report.size == report.expected_size;
    report.sha256 = katana::io::sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (options.expected_sha256.has_value()) {
        report.expected_sha256 = lower_ascii(*options.expected_sha256);
        report.hash_valid = report.sha256 == *report.expected_sha256;
    }
    report.sensitive_values_included = options.include_sensitive;
    if (!report.size_valid) return report;

    if (kind == FirmwareImageKind::Bios) {
        report.ranges.push_back({0u, bytes.size(), FirmwareRangeKind::Unknown});
        return report;
    }

    report.ranges = {{0x00000u, 0x10000u, FirmwareRangeKind::Data},
                     {0x10000u, 0x08000u, FirmwareRangeKind::Data},
                     {0x18000u, 0x02000u, FirmwareRangeKind::Data},
                     {0x1A000u, 0x02000u, FirmwareRangeKind::Data},
                     {0x1C000u, 0x04000u, FirmwareRangeKind::Data}};
    report.partitions.reserve(flash_layout.size());
    for (const auto& layout : flash_layout) {
        report.partitions.push_back(inspect_partition(bytes, layout));
    }
    if (options.include_sensitive) {
        report.sensitive_region_code = region_code(bytes, 0x1A000u);
    }
    return report;
}

std::string format_firmware_diagnostic_json(const FirmwareDiagnosticReport& report) {
    std::ostringstream output;
    katana::io::write_json_report_header(output,
                                         "katana-firmware-diagnostic",
                                         "firmware-diagnostic",
                                         report.valid() ? "success" : "failed");
    output << ",\"diagnostic_version\":" << firmware_diagnostic_schema_version
           << ",\"kind\":" << katana::io::quote_json(firmware_image_kind_name(report.kind))
           << ",\"size\":" << report.size << ",\"expected_size\":" << report.expected_size
           << ",\"size_valid\":" << (report.size_valid ? "true" : "false")
           << ",\"sha256\":" << katana::io::quote_json(report.sha256) << ",\"expected_sha256\":";
    report.expected_sha256.has_value() ? output << katana::io::quote_json(*report.expected_sha256)
                                       : output << "null";
    output << ",\"hash_valid\":" << (report.hash_valid ? "true" : "false") << ",\"read_only\":true"
           << ",\"portable\":" << (report.sensitive_values_included ? "false" : "true")
           << ",\"sensitive_values_included\":"
           << (report.sensitive_values_included ? "true" : "false")
           << ",\"sensitive_region_code\":";
    report.sensitive_region_code.has_value()
        ? output << katana::io::quote_json(*report.sensitive_region_code)
        : output << "null";
    output << ",\"ranges\":[";
    for (std::size_t index = 0u; index < report.ranges.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& range = report.ranges[index];
        output << "{\"offset\":" << range.offset << ",\"size\":" << range.size
               << ",\"classification\":"
               << katana::io::quote_json(firmware_range_kind_name(range.kind)) << '}';
    }
    output << "],\"partitions\":[";
    for (std::size_t index = 0u; index < report.partitions.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& partition = report.partitions[index];
        output << "{\"id\":" << static_cast<unsigned>(partition.id)
               << ",\"offset\":" << partition.offset << ",\"size\":" << partition.size
               << ",\"block_allocated\":" << (partition.block_allocated ? "true" : "false")
               << ",\"header_valid\":";
        partition.block_allocated ? output << (partition.header_valid ? "true" : "false")
                                  : output << "null";
        output << ",\"allocated_records\":" << partition.allocated_records
               << ",\"valid_crc_records\":" << partition.valid_crc_records
               << ",\"invalid_crc_records\":" << partition.invalid_crc_records
               << ",\"logical_blocks\":[";
        for (std::size_t block = 0u; block < partition.logical_blocks.size(); ++block) {
            if (block != 0u) output << ',';
            const auto& logical = partition.logical_blocks[block];
            output << "{\"logical_id\":";
            report.sensitive_values_included ? output << logical.logical_id : output << "null";
            output << ",\"generations\":" << logical.generations
                   << ",\"latest_physical_block\":" << logical.latest_physical_block
                   << ",\"valid_crc_generations\":" << logical.valid_crc_generations << '}';
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

const char* firmware_image_kind_name(const FirmwareImageKind kind) noexcept {
    return kind == FirmwareImageKind::Bios ? "bios" : "flash";
}

const char* firmware_range_kind_name(const FirmwareRangeKind kind) noexcept {
    switch (kind) {
    case FirmwareRangeKind::Code:
        return "code";
    case FirmwareRangeKind::Data:
        return "data";
    case FirmwareRangeKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace katana::platform
