#include "katana/gui/model.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Fixture final {
    std::filesystem::path root = std::filesystem::current_path() / "katana-phase10-gui-fixture";
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
    using namespace katana;
    Fixture fixture;
    const auto source = fixture.root / "program.bin";
    {
        std::ofstream output(source, std::ios::binary);
        const char bytes[]{'\x09', '\x00', '\x0B', '\x00', '\x09', '\x00'};
        output.write(bytes, 6);
    }
    gui::Model model(fixture.root / "settings.conf");
    require(model.page() == gui::Page::Dashboard && !model.has_project(),
            "GUI-Shell startet nicht auf dem Projekt-Dashboard.");
    model.navigate_previous();
    require(model.page() == gui::Page::Settings, "Rueckwaertsnavigation ist nicht zyklisch.");
    model.navigate_next();
    require(model.page() == gui::Page::Dashboard,
            "Tastaturnavigation verliert die Fokusreihenfolge.");

    const auto manifest = fixture.root / "project.katana";
    model.new_project(manifest, "gui-project", io::ProjectInputFormat::RawBinary, source);
    require(model.has_project() && model.has_unsaved_changes(),
            "GUI-Projektanlage bildet ungespeicherte Aenderungen nicht ab.");
    model.save_project();
    require(!model.has_unsaved_changes() && std::filesystem::exists(manifest),
            "GUI-Projekt konnte nicht ohne manuellen Dateiedit gespeichert werden.");
    model.update_manifest(
        [](auto& manifest) { manifest.firmware_mode = io::ProjectFirmwareMode::Hle; });
    bool invalid_profile_rejected = false;
    try {
        model.save_project();
    } catch (const std::exception&) {
        invalid_profile_rejected = true;
    }
    require(invalid_profile_rejected,
            "GUI-Editor akzeptiert ein Firmwareprofil ohne erforderliche Kernfaehigkeit.");
    model.update_manifest(
        [](auto& manifest) { manifest.firmware_mode = io::ProjectFirmwareMode::Direct; });
    model.save_project();

    const auto first =
        model.run_job(app::JobKind::Build, fixture.root / "build-a", "gui-build-a", "0.40.0-dev");
    require(first.state == app::JobState::Completed && model.page() == gui::Page::Results,
            "GUI-Build erreicht Ergebnisansicht nicht.");
    const auto snapshot = model.snapshot();
    require(snapshot.project_name == "gui-project" && snapshot.source_name == "program.bin" &&
                !snapshot.artifacts.empty() && !snapshot.job_active,
            "GUI-Snapshot verliert Projekt, Quelle, Jobabschluss oder Artefakte.");
    const auto automation = model.automation_snapshot_json();
    require(automation.find("gui-project") != std::string::npos &&
                automation.find(fixture.root.generic_string()) == std::string::npos,
            "Automatisierbarer GUI-Snapshot ist unvollstaendig oder enthaelt Hostpfade.");
    require(model.accessible_summary().find("Projekt gui-project") != std::string::npos,
            "Zugaengliche Shell-Zusammenfassung benennt den Projektzustand nicht.");

    std::optional<app::JobResult> concurrent_result;
    std::jthread worker([&] {
        concurrent_result = model.run_job(
            app::JobKind::Build, fixture.root / "build-concurrent", "gui-concurrent", "0.40.0-dev");
    });
    for (std::size_t attempt = 0u; attempt < 2'000u && !model.snapshot().job_active; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(model.snapshot().job_active, "Paralleler GUI-Test beobachtet keinen aktiven Job.");
    model.navigate_next();
    static_cast<void>(model.snapshot());
    bool project_change_rejected = false;
    try {
        model.new_project(fixture.root / "forbidden.katana",
                          "forbidden",
                          io::ProjectInputFormat::RawBinary,
                          source);
    } catch (const std::logic_error&) {
        project_change_rejected = true;
    }
    model.cancel_job();
    worker.join();
    require(project_change_rejected && concurrent_result.has_value() &&
                concurrent_result->state == app::JobState::Cancelled,
            "Aktiver GUI-Job erlaubt Projektmutation oder ignoriert parallelen Abbruch.");

    gui::Model restored(fixture.root / "settings.conf");
    restored.open_project(manifest);
    require(restored.snapshot().project_name == "gui-project" &&
                restored.settings().recent_projects.size() == 1u,
            "Projektwechsel oder Neustart stellt Projekt und zuletzt verwendete Liste nicht wieder "
            "her.");

    std::cout << "KR_PHASE10_GUI_MODEL_SUCCESS\n";
    return EXIT_SUCCESS;
}
