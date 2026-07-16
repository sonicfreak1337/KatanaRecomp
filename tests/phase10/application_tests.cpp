#include "katana/app/application.hpp"
#include "katana/gui/model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t raw_sector_size = 2352u;
constexpr std::size_t payload_size = 2048u;
constexpr std::uint32_t data_lba = 100u;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function> void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

struct Fixture final {
    std::filesystem::path root = std::filesystem::current_path() / "katana-phase10-app-fixture";
    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "disc");
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void write_binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void both32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        bytes[offset + 4u + index] = static_cast<std::uint8_t>(value >> ((3u - index) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& bytes,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    bytes[offset] = length;
    both32(bytes, offset + 2u, lba);
    both32(bytes, offset + 10u, size);
    bytes[offset + 25u] = directory ? 2u : 0u;
    bytes[offset + 28u] = 1u;
    bytes[offset + 31u] = 1u;
    bytes[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

std::size_t payload_offset(const std::size_t sector, const std::size_t byte = 0u) {
    return sector * raw_sector_size + 16u + byte;
}

std::vector<std::uint8_t> boot_track() {
    std::vector<std::uint8_t> bytes(22u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 22u; ++sector)
        bytes[sector * raw_sector_size + 15u] = 1u;
    const std::string hardware = "SEGA SEGAKATANA ";
    const std::string boot_file = "BOOT.BIN        ";
    std::copy(hardware.begin(),
              hardware.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u)));
    std::copy(boot_file.begin(),
              boot_file.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x60u)));
    const auto pvd = payload_offset(16u);
    bytes[pvd] = 1u;
    std::copy_n("CD001", 5u, bytes.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    bytes[pvd + 6u] = 1u;
    record(bytes, pvd + 156u, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    record(bytes, directory, data_lba + 21u, 8u, "BOOT.BIN;1", false);
    constexpr std::array<std::uint8_t, 8u> program = {
        0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u};
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

std::filesystem::path write_gdi_fixture(const std::filesystem::path& directory) {
    write_binary(directory / "low.bin", std::vector<std::uint8_t>(24u * raw_sector_size));
    write_binary(directory / "audio.raw", std::vector<std::uint8_t>(raw_sector_size));
    write_binary(directory / "high.bin", boot_track());
    const auto descriptor = directory / "disc.gdi";
    write_text(descriptor,
               "3\n1 0 4 2352 low.bin 0\n2 30 0 2352 audio.raw 0\n3 100 4 2352 high.bin 0\n");
    return descriptor;
}

katana::io::ProjectManifest raw_manifest(const std::filesystem::path& input) {
    katana::io::ProjectManifest manifest;
    manifest.project_name = "phase10-raw";
    manifest.format = katana::io::ProjectInputFormat::RawBinary;
    manifest.input_path = input;
    manifest.base_address = 0x8C010000u;
    manifest.entry_point = 0x8C010000u;
    manifest.segment_name = ".text";
    return manifest;
}

} // namespace

int main() {
    using namespace katana;
    Fixture fixture;
    const auto raw = fixture.root / "program.bin";
    write_binary(raw, {0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u});
    const auto manifest_path = fixture.root / "project.katana";
    auto session = app::ProjectSession::create(manifest_path, raw_manifest(raw));
    require(session.dirty(), "Neues Projekt ist nicht als ungespeichert markiert.");
    session.save();
    require(!session.dirty() && io::parse_project_manifest(manifest_path).input_path == raw,
            "Projekt wurde nicht atomar und relativ gespeichert.");

    app::ApplicationService service;
    const auto inspection = service.inspect_source(session.manifest());
    require(inspection.format == "raw" && inspection.display_name == "program.bin" &&
                inspection.size == 6u && inspection.sha256.size() == 64u,
            "Raw-Quellinspektion verliert Format, portable Anzeige oder Identitaet.");

    std::string expected_identity;
    for (const auto kind : {app::JobKind::Validate,
                            app::JobKind::Analyze,
                            app::JobKind::Codegen,
                            app::JobKind::Build,
                            app::JobKind::RunPreflight}) {
        const auto name = std::string(app::job_kind_name(kind));
        std::vector<app::JobEvent> events;
        const auto result =
            service.execute({name, kind, manifest_path, fixture.root / name, "0.40.0-dev"},
                            {},
                            [&](const app::JobEvent& event) { events.push_back(event); });
        require(result.state == app::JobState::Completed && !result.artifacts.empty() &&
                    events.front().state == app::JobState::Queued &&
                    events.back().state == app::JobState::Completed,
                "Gemeinsamer Jobdienst verliert Zustand, Ereignisse oder Artefakte fuer " + name +
                    '.');
        if (expected_identity.empty()) expected_identity = result.project_identity;
        require(result.project_identity == expected_identity,
                "GUI-/CLI-Jobarten verwenden keine gemeinsame Projektidentitaet.");
    }

    auto cancelled = std::make_shared<app::Cancellation>();
    cancelled->request();
    const auto cancelled_result = service.execute(
        {"cancelled", app::JobKind::Build, manifest_path, fixture.root / "cancelled", "0.40.0-dev"},
        cancelled);
    require(cancelled_result.state == app::JobState::Cancelled &&
                !std::filesystem::exists(fixture.root / "cancelled" / "generated"),
            "Kontrollierter Abbruch hinterlaesst generierte Teilartefakte.");

    const auto gdi = write_gdi_fixture(fixture.root / "disc");
    auto gdi_manifest = raw_manifest(gdi);
    gdi_manifest.project_name = "phase10-gdi";
    gdi_manifest.format = io::ProjectInputFormat::DreamcastGdi;
    gdi_manifest.base_address.reset();
    gdi_manifest.entry_point = 0x8C010000u;
    const auto gdi_manifest_path = fixture.root / "disc-project.katana";
    auto gdi_session = app::ProjectSession::create(gdi_manifest_path, gdi_manifest);
    gdi_session.save();
    const auto reopened = app::ProjectSession::open(gdi_manifest_path);
    const auto gdi_inspection = service.inspect_source(reopened.manifest());
    require(reopened.manifest().format == io::ProjectInputFormat::DreamcastGdi &&
                gdi_inspection.tracks.size() == 3u && gdi_inspection.tracks[1].role == "audio" &&
                gdi_inspection.tracks[2].descriptor_line == 4u,
            "GDI-Projekt oder Trackinspektor verliert Rollen, Reihenfolge oder Zeilenprovenienz.");
    const auto gdi_job = service.execute({"gdi-build",
                                          app::JobKind::Build,
                                          gdi_manifest_path,
                                          fixture.root / "gdi-build",
                                          "0.40.0-dev"});
    require(gdi_job.state == app::JobState::Completed &&
                std::find(gdi_job.checkpoints.begin(),
                          gdi_job.checkpoints.end(),
                          "build-project-ready") != gdi_job.checkpoints.end(),
            "Synthetische GDI erreicht den gemeinsamen GUI-/CLI-Buildpfad nicht.");
    gui::Model gui_model(fixture.root / "gui-settings.conf");
    gui_model.open_project(gdi_manifest_path);
    const auto gui_job = gui_model.run_job(
        app::JobKind::Build, fixture.root / "gdi-gui-build", "gdi-gui-build", "0.40.0-dev");
    require(gui_job.state == app::JobState::Completed &&
                gui_job.project_identity == gdi_job.project_identity &&
                gui_model.page() == gui::Page::Results,
            "GUI und direkter Anwendungsdienst erzeugen fuer GDI keine identische Identitaet.");

    write_text(fixture.root / "disc" / "disc.gdi", "1\n1 0 4 2352 missing.bin 0\n");
    const auto failed = service.execute({"gdi-error",
                                         app::JobKind::Validate,
                                         gdi_manifest_path,
                                         fixture.root / "gdi-error",
                                         "0.40.0-dev"});
    require(failed.state == app::JobState::Failed && failed.diagnostics.size() == 1u &&
                failed.diagnostics.front().code == "job-failed",
            "GDI-Fehler wird nicht als strukturierter gemeinsamer Jobfehler gemeldet.");

    const auto windows_path =
        std::string("C:") + '\\' + "Users" + '\\' + "name" + '\\' + "secret.bin";
    const auto posix_path = std::string("/") + "home" + "/name/trace.bin";
    const auto redacted = app::redact_sensitive_text(windows_path + " firmware_bytes " +
                                                     posix_path + " serial_number");
    require(redacted.find("Users") == std::string::npos &&
                redacted.find("firmware_bytes") == std::string::npos &&
                redacted.find(posix_path) == std::string::npos &&
                redacted.find("serial_number") == std::string::npos,
            "Diagnoseredaktion behaelt Hostpfade oder sensible Felder.");

    app::UserSettings settings;
    settings.theme = "dark";
    settings.scale_percent = 175u;
    app::remember_recent_project(settings, manifest_path);
    const auto settings_path = fixture.root / "settings.conf";
    app::save_user_settings(settings_path, settings);
    const auto loaded_settings = app::load_user_settings(settings_path);
    require(loaded_settings.theme == "dark" && loaded_settings.scale_percent == 175u &&
                loaded_settings.recent_projects.size() == 1u,
            "Versionierte GUI-Einstellungen werden nicht stabil wiederhergestellt.");
    write_text(settings_path, "version=99\n");
    require_failure([&] { static_cast<void>(app::load_user_settings(settings_path)); },
                    "Unbekannte Einstellungsversion wurde akzeptiert.");

    std::cout << "KR_PHASE10_APPLICATION_SERVICE_SUCCESS\n"
              << "KR_PHASE10_GUI_END_TO_END\n";
    return EXIT_SUCCESS;
}
