#include "katana/platform/firmware_profile.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function> std::string require_rejection(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    require(false, "Nicht verfuegbares Firmwareprofil wurde akzeptiert.");
    return {};
}

} // namespace

int main() {
    using namespace katana::platform;
    const auto contracts = alpha_firmware_contracts();
    require(
        contracts.size() == 3u && contracts[0].id == "direct" &&
            contracts[0].status == FirmwareProfileStatus::Available && contracts[0].retail_boot &&
            contracts[1].id == "hle" && contracts[1].status == FirmwareProfileStatus::Available &&
            contracts[2].id == "lle" && contracts[2].status == FirmwareProfileStatus::Unsupported,
        "Alpha-Firmwarematrix behauptet falsche Verfuegbarkeit.");
    require(contracts[0].source_inputs_read_only && !contracts[0].source_inputs_packaged &&
                contracts[1].source_inputs_read_only && !contracts[1].source_inputs_packaged &&
                contracts[1].mutable_flash_uses_working_copy &&
                contracts[2].source_inputs_read_only && !contracts[2].source_inputs_packaged,
            "Firmwarequellen sind nicht durchgaengig read-only und portextern.");

    require_alpha_firmware_profile(FirmwareMode::DirectHomebrew);
    require_alpha_firmware_profile(FirmwareMode::HleBiosAbi);
    auto error = std::string{};
    AlphaFirmwareInputPolicy lle_inputs;
    lle_inputs.bios_source = std::filesystem::temp_directory_path() / "katana-local-bios.bin";
    error = require_rejection(
        [&] { require_alpha_firmware_profile(FirmwareMode::LleFirmware, lle_inputs); });
    require(error.find("unsupported") != std::string::npos &&
                error.find("not implemented") != std::string::npos,
            "Optionales LLE wird nicht ehrlich abgelehnt.");

    const auto root = std::filesystem::temp_directory_path() / "katana-alpha-firmware-contract";
    AlphaFirmwareInputPolicy leaked;
    leaked.bios_source = root / "port" / "firmware" / "bios.bin";
    leaked.port_output_directory = root / "port";
    error = require_rejection(
        [&] { require_alpha_firmware_profile(FirmwareMode::LleFirmware, leaked); });
    require(error.find("ausserhalb") != std::string::npos,
            "Firmwarequelle im Port-Ausgabeordner wurde nicht vor Zugriff abgelehnt.");

    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    std::filesystem::create_directories(root / "port", filesystem_error);
    std::filesystem::create_directories(root / "private", filesystem_error);
    std::ofstream(root / "private" / "bios.bin", std::ios::binary).put('\0');
    std::filesystem::create_directory_symlink(
        root / "private", root / "port" / "firmware-link", filesystem_error);
    if (!filesystem_error) {
        AlphaFirmwareInputPolicy outward_link;
        outward_link.bios_source = root / "port" / "firmware-link" / "bios.bin";
        outward_link.port_output_directory = root / "port";
        error = require_rejection(
            [&] { require_alpha_firmware_profile(FirmwareMode::LleFirmware, outward_link); });
        require(error.find("ausserhalb") != std::string::npos,
                "Symlink beziehungsweise Reparse Point aus dem Port wurde nicht erkannt.");
    }
    filesystem_error.clear();
    std::filesystem::create_directory_symlink(
        root / "port", root / "private" / "port-link", filesystem_error);
    if (!filesystem_error) {
        AlphaFirmwareInputPolicy inward_link;
        inward_link.bios_source = root / "private" / "port-link" / "not-yet-created.bin";
        inward_link.port_output_directory = root / "port";
        error = require_rejection(
            [&] { require_alpha_firmware_profile(FirmwareMode::LleFirmware, inward_link); });
        require(
            error.find("ausserhalb") != std::string::npos,
            "Externer Symlink in den Port oder nicht vorhandener Kindpfad wurde nicht erkannt.");
    }

    AlphaFirmwareInputPolicy flash;
    std::ofstream(root / "private" / "flash.bin", std::ios::binary).put('\0');
    std::filesystem::create_directories(root / "private" / "working", filesystem_error);
    flash.flash_source = root / "private" / "flash.bin";
    flash.port_output_directory = root / "port";
    error =
        require_rejection([&] { require_alpha_firmware_profile(FirmwareMode::HleBiosAbi, flash); });
    require(error.find("Arbeitskopie") != std::string::npos,
            "Veraenderliche Flash-Nutzung ohne kontrollierte Arbeitskopie lieferte eine falsche "
            "Diagnose: " +
                error);
    flash.mutable_working_directory = root / "private" / "working";
    require_alpha_firmware_profile(FirmwareMode::HleBiosAbi, flash);

    const auto json = format_alpha_firmware_contract_json();
    require(json.find("\"schema\":\"katana-alpha-firmware\"") != std::string::npos &&
                json.find("\"source_inputs_read_only\":true") != std::string::npos &&
                json.find("\"source_inputs_packaged\":false") != std::string::npos &&
                json.find("\"status\":\"available\"") != std::string::npos &&
                json.find("Synthetic ABI vector installation") != std::string::npos,
            "Maschinenlesbarer Firmwarevertrag ist unvollstaendig.");

    std::cout << "KR-4601 Alpha-Firmware- und Retail-Bootvertrag erfolgreich.\n";
}
