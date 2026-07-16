#include "katana/io/input_provenance.hpp"
#include "katana/platform/firmware_diagnostics.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::uint16_t crc_record(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    std::uint32_t crc = 0xFFFFu;
    for (std::size_t index = 0u; index < 62u; ++index) {
        crc ^= static_cast<std::uint32_t>(bytes[offset + index]) << 8u;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u ? (crc << 1u) ^ 0x1021u : crc << 1u;
            crc &= 0xFFFFu;
        }
    }
    return static_cast<std::uint16_t>((~crc) & 0xFFFFu);
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

struct Fixture final {
    std::filesystem::path root =
        std::filesystem::current_path() / "katana-firmware-diagnostic-fixture";

    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

} // namespace

int main() {
    using namespace katana::platform;
    Fixture fixture;

    const auto short_path = fixture.root / "synthetic-short.bin";
    write_file(short_path, std::vector<std::uint8_t>(128u, 0x5Au));
    const auto short_report = inspect_firmware_file(short_path, FirmwareImageKind::Bios);
    require(!short_report.size_valid && !short_report.valid() &&
                short_report.expected_size == katana::runtime::dreamcast_bios_size,
            "Falsche BIOS-Groesse wurde nicht sichtbar abgelehnt.");

    const auto flash_path = fixture.root / "synthetic-flash.bin";
    std::vector<std::uint8_t> flash(katana::runtime::dreamcast_flash_size, 0xFFu);
    constexpr std::size_t partition = 0x00000u;
    constexpr std::size_t record_size = 64u;
    constexpr std::size_t bitmap = 0x10000u - 128u;
    const std::string magic = "KATANA_FLASH____";
    std::copy(magic.begin(), magic.end(), flash.begin() + partition);
    flash[partition + magic.size()] = 4u;
    flash[partition + magic.size() + 1u] = 0u;

    const auto first_record = partition + record_size;
    flash[first_record] = 0x34u;
    flash[first_record + 1u] = 0x12u;
    flash[first_record + 2u] = 0xA5u;
    const auto first_crc = crc_record(flash, first_record);
    flash[first_record + 62u] = static_cast<std::uint8_t>(first_crc);
    flash[first_record + 63u] = static_cast<std::uint8_t>(first_crc >> 8u);

    const auto second_record = partition + 2u * record_size;
    flash[second_record] = 0x34u;
    flash[second_record + 1u] = 0x12u;
    flash[second_record + 2u] = 0x5Au;
    flash[second_record + 62u] = 0u;
    flash[second_record + 63u] = 0u;
    flash[bitmap] = 0x3Fu; // records zero and one are allocated

    const std::string region = "EUROP";
    std::copy(region.begin(), region.end(), flash.begin() + 0x1A000u);
    write_file(flash_path, flash);
    const auto before_hash = katana::io::capture_input_provenance("flash", flash_path).sha256;

    const auto report = inspect_firmware_file(flash_path, FirmwareImageKind::Flash);
    const auto part4 = std::find_if(report.partitions.begin(),
                                    report.partitions.end(),
                                    [](const auto& value) { return value.id == 4u; });
    require(report.valid() && report.read_only && part4 != report.partitions.end() &&
                part4->header_valid && part4->allocated_records == 2u &&
                part4->valid_crc_records == 1u && part4->invalid_crc_records == 1u &&
                part4->logical_blocks.size() == 1u && part4->logical_blocks[0].generations == 2u &&
                part4->logical_blocks[0].latest_physical_block == 2u,
            "Flashheader, Bitmap, Generationen oder CRCs wurden falsch diagnostiziert.");
    const auto portable = format_firmware_diagnostic_json(report);
    require(portable.find("EUROP") == std::string::npos &&
                portable.find("\"logical_id\":null") != std::string::npos &&
                portable.find("\"portable\":true") != std::string::npos,
            "Portable Flashdiagnose redigiert sensible Werte nicht.");

    const auto sensitive =
        inspect_firmware_file(flash_path,
                              FirmwareImageKind::Flash,
                              {.expected_sha256 = before_hash, .include_sensitive = true});
    const auto sensitive_json = format_firmware_diagnostic_json(sensitive);
    require(sensitive.valid() && sensitive.sensitive_region_code == region &&
                sensitive_json.find("EUROP") != std::string::npos &&
                sensitive_json.find("\"logical_id\":4660") != std::string::npos &&
                sensitive_json.find("\"portable\":false") != std::string::npos,
            "Explizites lokales Opt-in gibt sensible synthetische Diagnosewerte nicht frei.");

    const auto wrong_hash = inspect_firmware_file(
        flash_path,
        FirmwareImageKind::Flash,
        {.expected_sha256 = std::string(64u, '0'), .include_sensitive = false});
    require(wrong_hash.size_valid && !wrong_hash.hash_valid && !wrong_hash.valid(),
            "Falscher deklarierter Firmwarehash wird nicht abgelehnt.");
    require(katana::io::capture_input_provenance("flash", flash_path).sha256 == before_hash,
            "Read-only-Firmwarediagnose hat die Eingabedatei veraendert.");

    std::cout << "KR-3606 sichere Firmware- und Flashdiagnostik erfolgreich.\n";
    return EXIT_SUCCESS;
}
