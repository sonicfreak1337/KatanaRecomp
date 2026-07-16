#include "katana/app/application.hpp"
#include "katana/gui/model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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
        std::filesystem::create_directories(root / "forbidden-source-root");
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

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

    app::ApplicationService service(std::filesystem::current_path().parent_path());
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
                    result.failure_category == app::JobFailureCategory::None &&
                    events.front().state == app::JobState::Queued &&
                    events.back().state == app::JobState::Completed,
                "Gemeinsamer Jobdienst verliert Zustand, Ereignisse oder Artefakte fuer " + name +
                    '.');
        if (expected_identity.empty()) expected_identity = result.project_identity;
        require(result.project_identity == expected_identity,
                "GUI-/CLI-Jobarten verwenden keine gemeinsame Projektidentitaet.");
        require(result.tool_version == "0.40.0-dev" &&
                    app::format_job_result_json(result).find("\"tool_version\":\"0.40.0-dev\"") !=
                        std::string::npos,
                "Job und JSON verlieren die kanonische Werkzeugversion.");
    }

    auto unsupported_profile = raw_manifest(raw);
    unsupported_profile.fallback_policy = io::ProjectFallbackPolicy::Interpreter;
    unsupported_profile.required_backend_capabilities = {"controlled-fallback"};
    const auto unsupported_manifest = fixture.root / "unsupported-profile.katana";
    auto unsupported_session =
        app::ProjectSession::create(unsupported_manifest, unsupported_profile);
    unsupported_session.save();
    const auto unsupported_job = service.execute({"unsupported-profile",
                                                  app::JobKind::Codegen,
                                                  unsupported_manifest,
                                                  fixture.root / "unsupported-profile",
                                                  "0.40.0-dev"});
    require(unsupported_job.state == app::JobState::Failed &&
                unsupported_job.failure_category == app::JobFailureCategory::CodeGeneration &&
                !unsupported_job.diagnostics.empty() &&
                unsupported_job.diagnostics.back().message.find("controlled-fallback") !=
                    std::string::npos,
            "Gemeinsamer Dienst akzeptiert ein vom C++-Backend nicht anwendbares Profil.");

    const auto incomplete_raw = fixture.root / "incomplete.bin";
    std::vector<std::uint8_t> incomplete_program(2u * 1024u * 1024u);
    incomplete_program[0] = 0x0Bu;
    incomplete_program[1] = 0x00u;
    incomplete_program[2] = 0x09u;
    incomplete_program[3] = 0x00u;
    write_binary(incomplete_raw, incomplete_program);
    const auto incomplete_manifest_path = fixture.root / "incomplete.katana";
    auto incomplete_session =
        app::ProjectSession::create(incomplete_manifest_path, raw_manifest(incomplete_raw));
    incomplete_session.save();
    std::vector<app::JobEvent> incomplete_events;
    const auto incomplete_result =
        service.execute({"incomplete",
                         app::JobKind::Build,
                         incomplete_manifest_path,
                         fixture.root / "incomplete",
                         "0.40.0-dev"},
                        {},
                        [&](const app::JobEvent& event) { incomplete_events.push_back(event); });
    const auto incomplete_json = app::format_job_result_json(incomplete_result);
    const auto incomplete_plan = fixture.root / "incomplete" / "build-plan.json";
    require(incomplete_result.state == app::JobState::Partial &&
                incomplete_result.failure_category == app::JobFailureCategory::None &&
                incomplete_result.analysis_coverage.has_value() &&
                incomplete_result.analysis_coverage->unresolved_control_flow == 0u &&
                incomplete_result.analysis_coverage->committed_executable_bytes ==
                    incomplete_program.size() &&
                incomplete_result.analysis_coverage->analyzed_instruction_bytes == 4u &&
                incomplete_result.analysis_coverage->unanalyzed_executable_bytes ==
                    incomplete_program.size() - 4u &&
                incomplete_result.analysis_coverage->reachable_abort_edges == 0u &&
                !incomplete_result.analysis_coverage->control_flow_complete &&
                incomplete_events.back().state == app::JobState::Partial &&
                incomplete_json.find("\"version\":4") != std::string::npos &&
                incomplete_json.find("\"state\":\"partial\"") != std::string::npos &&
                incomplete_json.find("\"unresolved_control_flow\":0") != std::string::npos &&
                incomplete_json.find("\"unanalyzed_executable_bytes\":2097148") !=
                    std::string::npos &&
                std::filesystem::exists(incomplete_plan) &&
                !std::filesystem::exists(fixture.root / "incomplete" / "generated") &&
                !std::filesystem::exists(fixture.root / "incomplete" / "game.exe"),
            "Unvollstaendige Analyse wird als erfolgreicher Build behandelt.");
    std::ifstream incomplete_plan_input(incomplete_plan, std::ios::binary);
    const std::string incomplete_plan_text((std::istreambuf_iterator<char>(incomplete_plan_input)),
                                           std::istreambuf_iterator<char>());
    require(incomplete_plan_text.find("\"status\":\"partial\"") != std::string::npos &&
                incomplete_plan_text.find("\"version\":4") != std::string::npos &&
                incomplete_plan_text.find("\"host_compilation\":false") != std::string::npos &&
                incomplete_plan_text.find("\"tool_version\":\"0.40.0-dev\"") != std::string::npos,
            "Partieller Buildplan verliert Zustand, Hostbuildgrenze oder Werkzeugversion.");

    const auto publication_race = fixture.root / "publication-race";
    std::vector<app::JobEvent> publication_events;
    const auto publication_failure = service.execute(
        {"publication-race",
         app::JobKind::Build,
         incomplete_manifest_path,
         publication_race,
         "0.40.0-dev"},
        {},
        [&](const app::JobEvent& event) {
            publication_events.push_back(event);
            if (event.stage == "finalization" && event.state == app::JobState::Running &&
                event.step_status == app::JobStepStatus::Running) {
                std::filesystem::create_directories(publication_race);
                write_text(publication_race / "concurrent-owner.txt", "occupied");
            }
        });
    require(publication_failure.state == app::JobState::Failed &&
                publication_failure.failure_category == app::JobFailureCategory::InputOutput &&
                !publication_events.empty() &&
                publication_events.back().state == app::JobState::Failed &&
                publication_events.back().step_status == app::JobStepStatus::Failed &&
                std::none_of(publication_events.begin(),
                             publication_events.end(),
                             [](const auto& event) {
                                 return event.state == app::JobState::Completed ||
                                        event.state == app::JobState::Partial;
                             }) &&
                std::any_of(publication_failure.diagnostics.begin(),
                            publication_failure.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code == "job-publication-failed";
                            }),
            "Fehlgeschlagene atomare Veroeffentlichung sendet vorher ein terminales Erfolgsevent.");

    const auto aborted_raw = fixture.root / "aborted-edge.bin";
    write_binary(aborted_raw, {0x09u, 0x00u});
    const auto aborted_manifest_path = fixture.root / "aborted-edge.katana";
    auto aborted_session =
        app::ProjectSession::create(aborted_manifest_path, raw_manifest(aborted_raw));
    aborted_session.save();
    const auto aborted_result = service.execute({"aborted-edge",
                                                 app::JobKind::Build,
                                                 aborted_manifest_path,
                                                 fixture.root / "aborted-edge",
                                                 "0.40.0-dev"});
    require(aborted_result.state == app::JobState::Partial &&
                aborted_result.analysis_coverage.has_value() &&
                aborted_result.analysis_coverage->unanalyzed_executable_bytes == 0u &&
                aborted_result.analysis_coverage->reachable_abort_edges == 1u &&
                !std::filesystem::exists(fixture.root / "aborted-edge" / "generated"),
            "Erreichbare Abbruchkante fehlt in Coverage oder gibt den Hostbuild frei.");

#ifndef _WIN32
    const auto lock_ready = fixture.root / "lock-ready";
    const auto lock_release = fixture.root / "lock-release";
    const auto locked_output = fixture.root / "cross-process-output";
    const auto child = ::fork();
    require(child >= 0, "Linux-Prozesslocktest konnte keinen Kindprozess starten.");
    if (child == 0) {
        const auto child_result =
            service.execute({"linux-lock-holder",
                             app::JobKind::Validate,
                             manifest_path,
                             locked_output,
                             "0.40.0-dev"},
                            {},
                            [&](const app::JobEvent& event) {
                                if (event.stage != "queued") return;
                                write_text(lock_ready, "ready\n");
                                while (!std::filesystem::exists(lock_release))
                                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            });
        ::_exit(child_result.state == app::JobState::Completed ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    for (std::size_t attempt = 0u; attempt < 500u && !std::filesystem::exists(lock_ready);
         ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(std::filesystem::exists(lock_ready),
            "Linux-Prozesslocktest beobachtet den gehaltenen Lock nicht.");
    require_failure(
        [&] {
            static_cast<void>(service.execute({"linux-lock-contender",
                                               app::JobKind::Validate,
                                               manifest_path,
                                               locked_output / "nested",
                                               "0.40.0-dev"}));
        },
        "Getrennte Linux-Prozesse akzeptieren ueberlappende Ausgabeziele.");
    write_text(lock_release, "release\n");
    int child_status = 0;
    require(::waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
                WEXITSTATUS(child_status) == EXIT_SUCCESS,
            "Linux-Prozesslockhalter wurde nicht sauber beendet.");
#endif

    auto cancelled = std::make_shared<app::Cancellation>();
    cancelled->request();
    const auto cancelled_result = service.execute(
        {"cancelled", app::JobKind::Build, manifest_path, fixture.root / "cancelled", "0.40.0-dev"},
        cancelled);
    require(cancelled_result.state == app::JobState::Cancelled &&
                cancelled_result.failure_category == app::JobFailureCategory::None &&
                !std::filesystem::exists(fixture.root / "cancelled" / "generated"),
            "Kontrollierter Abbruch hinterlaesst generierte Teilartefakte.");

    const auto gdi = write_gdi_fixture(fixture.root / "disc");
    auto gdi_manifest = raw_manifest(gdi);
    gdi_manifest.project_name = "phase10-gdi";
    gdi_manifest.format = io::ProjectInputFormat::DreamcastGdi;
    gdi_manifest.base_address.reset();
    gdi_manifest.entry_point = 0x8C010004u;
    gdi_manifest.expected_entry_points = {0x8C010000u};
    gdi_manifest.firmware_mode = io::ProjectFirmwareMode::Hle;
    gdi_manifest.required_backend_capabilities = {"memory", "firmware-mode"};
    const auto gdi_overrides = fixture.root / "disc" / "analysis.overrides";
    write_text(gdi_overrides,
               "version = 2\nschema = katana-analysis-directives\nmode = override\n"
               "function = 0x8C010002\n");
    gdi_manifest.analysis_overrides_path = gdi_overrides;
    const auto gdi_manifest_path = fixture.root / "disc-project.katana";
    auto gdi_session = app::ProjectSession::create(gdi_manifest_path, gdi_manifest);
    gdi_session.save();
    const auto reopened = app::ProjectSession::open(gdi_manifest_path);
    const auto gdi_inspection = service.inspect_source(reopened.manifest());
    require(reopened.manifest().format == io::ProjectInputFormat::DreamcastGdi &&
                gdi_inspection.tracks.size() == 3u && gdi_inspection.tracks[1].role == "audio" &&
                gdi_inspection.tracks[2].descriptor_line == 4u,
            "GDI-Projekt oder Trackinspektor verliert Rollen, Reihenfolge oder Zeilenprovenienz.");
    std::vector<app::JobEvent> gdi_events;
    const auto gdi_job =
        service.execute({"gdi-build",
                         app::JobKind::Build,
                         gdi_manifest_path,
                         fixture.root / "gdi-build",
                         "0.40.0-dev"},
                        {},
                        [&](const app::JobEvent& event) { gdi_events.push_back(event); });
    require(
        gdi_job.state == app::JobState::Completed &&
            std::find(gdi_job.checkpoints.begin(),
                      gdi_job.checkpoints.end(),
                      "host-build-complete") != gdi_job.checkpoints.end() &&
            std::filesystem::exists(fixture.root / "gdi-build" / "sourcecode" / "CMakeLists.txt") &&
            read_text(fixture.root / "gdi-build" / "sourcecode" / "src" / "main.cpp")
                    .find("DreamcastRuntimeFirmwareMode::HleBiosAbi") != std::string::npos &&
#ifdef _WIN32
            std::filesystem::exists(fixture.root / "gdi-build" / "game.exe"),
#else
            std::filesystem::exists(fixture.root / "gdi-build" / "game"),
#endif
        "Synthetische GDI erreicht den gemeinsamen GUI-/CLI-Buildpfad nicht: " +
            (gdi_job.diagnostics.empty() ? std::string("ohne Diagnose")
                                         : gdi_job.diagnostics.back().message));
    require(!gdi_events.empty() && gdi_events.front().sequence == 0u &&
                gdi_events.back().state == app::JobState::Completed,
            "Hierarchische GDI-Ereignisfolge besitzt keine stabilen Grenzen.");
    bool saw_indeterminate_configuration = false;
    bool saw_live_log = false;
    bool saw_compilation_log = false;
    for (std::size_t index = 0u; index < gdi_events.size(); ++index) {
        const auto& event = gdi_events[index];
        require(event.sequence == index &&
                    (index == 0u ||
                     (event.progress_percent >= gdi_events[index - 1u].progress_percent &&
                      event.timestamp_ms >= gdi_events[index - 1u].timestamp_ms &&
                      event.elapsed_ms >= gdi_events[index - 1u].elapsed_ms)),
                "Jobereignisse verlieren Sequenz, Zeitordnung oder monotonen Gesamtfortschritt.");
        if (event.step_total)
            require(event.step_current && *event.step_current <= *event.step_total,
                    "Bekannter Einzelschritt besitzt ungueltige Zaehler.");
        if (event.stage == "host-configuration" &&
            event.step_status == app::JobStepStatus::Running && !event.step_total)
            saw_indeterminate_configuration = true;
        if (event.log_chunk && !event.log_chunk->empty()) saw_live_log = true;
        if (event.stage == "host-compilation" && event.log_chunk && !event.log_chunk->empty())
            saw_compilation_log = true;
    }
    require(saw_indeterminate_configuration && saw_live_log && saw_compilation_log &&
                app::format_job_event_json(gdi_events.front()).find("\"step_total\":null") !=
                    std::string::npos,
            "Unbestimmter Hostschritt oder inkrementelles Live-Log fehlt.");
    auto changed_track = boot_track();
    changed_track[payload_offset(21u)] = 0x08u;
    const auto snapshot_race = service.execute(
        {"snapshot-race",
         app::JobKind::Analyze,
         gdi_manifest_path,
         fixture.root / "snapshot-race",
         "0.40.0-dev"},
        {},
        [&](const app::JobEvent& event) {
            if (event.stage == "hashing" && event.step_status == app::JobStepStatus::Completed)
                write_binary(fixture.root / "disc" / "high.bin", changed_track);
        });
    require(snapshot_race.state == app::JobState::Failed && !snapshot_race.diagnostics.empty() &&
                snapshot_race.diagnostics.back().message.find("Snapshot") != std::string::npos,
            "Geladenes GDI-Bootimage kann von der ausgewiesenen Provenienz abweichen.");
    write_binary(fixture.root / "disc" / "high.bin", boot_track());
    const auto port_metadata = read_text(fixture.root / "gdi-build" / "sourcecode" / "generated" /
                                         "metadata" / "port-project.json");
    const auto result_index = read_text(fixture.root / "gdi-build" / "result-index.json");
    std::string generated_code;
    for (const auto& entry : std::filesystem::directory_iterator(
             fixture.root / "gdi-build" / "sourcecode" / "generated" / "code")) {
        generated_code += read_text(entry.path());
    }
    require(port_metadata.find("\"entry_address\":2348875780") != std::string::npos &&
                port_metadata.find(gdi_job.project_identity) != std::string::npos &&
                result_index.find(gdi_job.project_identity) != std::string::npos &&
                generated_code.find("generated_entry_address = 0x8C010004u") != std::string::npos,
            "Analyse, Portmetadaten und generierter Einstieg verlieren die Projektidentitaet.");

    auto without_override = gdi_manifest;
    without_override.analysis_overrides_path.reset();
    const auto without_override_manifest = fixture.root / "disc-project-without-override.katana";
    auto without_override_session =
        app::ProjectSession::create(without_override_manifest, without_override);
    without_override_session.save();
    const auto without_override_job = service.execute({"gdi-without-override",
                                                       app::JobKind::Analyze,
                                                       without_override_manifest,
                                                       fixture.root / "gdi-without-override",
                                                       "0.40.0-dev"});
    require(without_override_job.state == app::JobState::Completed &&
                without_override_job.project_identity != gdi_job.project_identity &&
                read_text(fixture.root / "gdi-without-override" / "analysis.json") !=
                    read_text(fixture.root / "gdi-build" / "analysis.json"),
            "Override-Inhalt aendert weder Projektidentitaet noch Analyseausgabe.");

    auto host_cancel = std::make_shared<app::Cancellation>();
    const auto cancelled_host_root = fixture.root / "cancelled-host-build";
    const auto cancelled_host =
        service.execute({"cancelled-host",
                         app::JobKind::Build,
                         gdi_manifest_path,
                         cancelled_host_root,
                         "0.40.0-dev"},
                        host_cancel,
                        [&](const app::JobEvent& event) {
                            if (event.stage == "host-configuration" &&
                                event.step_status == app::JobStepStatus::Running)
                                host_cancel->request();
                        });
    require(cancelled_host.state == app::JobState::Cancelled &&
                std::filesystem::exists(cancelled_host_root / "job-result.json") &&
                !std::filesystem::exists(cancelled_host_root / "sourcecode") &&
                !std::filesystem::exists(cancelled_host_root / "recompile.log") &&
                !std::filesystem::exists(cancelled_host_root / "game.exe") &&
                std::none_of(std::filesystem::directory_iterator(fixture.root),
                             std::filesystem::directory_iterator{},
                             [](const auto& entry) {
                                 return entry.path().filename().string().starts_with(
                                     ".katana-stage-");
                             }),
            "Hostbuild-Abbruch hinterlaesst aktive oder gestagte Teilartefakte.");
    gui::Model gui_model(fixture.root / "gui-settings.conf");
    gui_model.open_project(gdi_manifest_path);
    const auto gui_job = gui_model.run_job(
        app::JobKind::Analyze, fixture.root / "gdi-gui-build", "gdi-gui-build", "0.40.0-dev");
    const auto gui_snapshot = gui_model.snapshot();
    require(gui_job.state == app::JobState::Completed &&
                gui_job.project_identity == gdi_job.project_identity &&
                gui_model.page() == gui::Page::Results && !gui_snapshot.job_events.empty() &&
                gui_snapshot.job_events.back().stage == "finalization",
            "GUI und direkter Anwendungsdienst erzeugen fuer GDI keine identische Identitaet.");

    write_text(fixture.root / "disc" / "disc.gdi", "1\n1 0 4 2352 missing.bin 0\n");
    const auto failed_rebuild = service.execute({"failed-rebuild",
                                                 app::JobKind::Build,
                                                 gdi_manifest_path,
                                                 fixture.root / "gdi-build",
                                                 "0.40.0-dev"});
    require(
        failed_rebuild.state == app::JobState::Failed &&
            std::filesystem::exists(fixture.root / "gdi-build" / "job-result.json") &&
            !std::filesystem::exists(fixture.root / "gdi-build" / "game.exe") &&
            !std::filesystem::exists(fixture.root / "gdi-build" / "sourcecode") &&
            std::filesystem::exists(std::filesystem::path((fixture.root / "gdi-build").string() +
                                                          ".katana-stale-failed-rebuild") /
#ifdef _WIN32
                                    "game.exe"),
#else
                                    "game"),
#endif
        "Fehlgeschlagene Wiederholung laesst alte oder teilweise Resultate aktiv.");
    const auto stale_root = std::filesystem::path((fixture.root / "gdi-build").string() +
                                                  ".katana-stale-failed-rebuild");
#ifdef _WIN32
    const auto stale_executable = stale_root / "game.exe";
#else
    const auto stale_executable = stale_root / "game";
#endif
    const auto stale_hash = read_text(stale_executable);
    const auto second_failed_rebuild = service.execute({"failed-rebuild",
                                                        app::JobKind::Build,
                                                        gdi_manifest_path,
                                                        fixture.root / "gdi-build",
                                                        "0.40.0-dev"});
    require(second_failed_rebuild.state == app::JobState::Failed &&
                std::filesystem::exists(stale_executable) &&
                read_text(stale_executable) == stale_hash,
            "Zweiter Fehler mit gleicher CLI-Job-ID loescht den letzten erfolgreichen Build.");
    const auto failed = service.execute({"gdi-error",
                                         app::JobKind::Validate,
                                         gdi_manifest_path,
                                         fixture.root / "gdi-error",
                                         "0.40.0-dev"});
    require(failed.state == app::JobState::Failed &&
                failed.failure_category == app::JobFailureCategory::InputOutput &&
                failed.diagnostics.size() == 1u && failed.diagnostics.front().code == "job-failed",
            "GDI-Fehler wird nicht als strukturierter gemeinsamer Jobfehler gemeldet.");

    const auto windows_path =
        std::string("C:") + '\\' + "Users" + '\\' + "name" + '\\' + "secret.bin";
    const auto posix_path = std::string("/") + "home" + "/name/trace.bin";
    const auto temporary_path = std::string("/") + "tmp" + "/ci/trace.bin";
    const auto redacted =
        app::redact_sensitive_text(windows_path + " firmware_bytes " + posix_path + " " +
                                   temporary_path + " serial_number /W4");
    require(redacted.find("Users") == std::string::npos &&
                redacted.find("firmware_bytes") == std::string::npos &&
                redacted.find(posix_path) == std::string::npos &&
                redacted.find(temporary_path) == std::string::npos &&
                redacted.find("serial_number") == std::string::npos &&
                redacted.find("/W4") != std::string::npos,
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
              << "KR_PHASE10_GUI_MODEL_INTEGRATION\n";
    return EXIT_SUCCESS;
}
