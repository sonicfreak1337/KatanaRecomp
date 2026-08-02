#include "port_build_telemetry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(
    const bool condition,
    const std::string& message) {
    if (condition) return;
    std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

[[nodiscard]] std::vector<std::string> read_lines(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(
        static_cast<bool>(input),
        "JSONL-Telemetriedatei wurde nicht erzeugt.");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
        lines.push_back(std::move(line));
    require(
        input.eof(),
        "JSONL-Telemetriedatei konnte nicht vollstaendig gelesen werden.");
    return lines;
}

void require_gapless_sequence(
    const std::vector<std::string>& lines) {
    std::uint64_t previous_elapsed = 0u;
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        const auto sequence =
            "\"sequence\":" + std::to_string(index);
        require(
            lines[index].find(sequence) != std::string::npos,
            "Writersequenz ist nicht lueckenlos.");
        constexpr std::string_view elapsed_field{"\"elapsed_ms\":"};
        const auto elapsed_begin = lines[index].find(elapsed_field);
        require(
            elapsed_begin != std::string::npos,
            "Writerrecord besitzt keinen elapsed_ms-Zeitstempel.");
        const auto value_begin = elapsed_begin + elapsed_field.size();
        const auto value_end = lines[index].find(',', value_begin);
        require(
            value_end != std::string::npos,
            "Writerrecord besitzt einen ungueltigen elapsed_ms-Zeitstempel.");
        const auto elapsed = static_cast<std::uint64_t>(
            std::stoull(lines[index].substr(
                value_begin, value_end - value_begin)));
        require(
            elapsed >= previous_elapsed,
            "Writerreihenfolge und elapsed_ms sind nicht monoton.");
        previous_elapsed = elapsed;
    }
}

[[nodiscard]] katana::cli::PortBuildResolvedEnvironment
resolved_environment() {
    katana::cli::PortBuildResolvedEnvironment environment;
    environment.compiler_identity = "MSVC";
    environment.compiler_version = "19.44.35228.0";
    environment.linker_identity = "MSVC";
    environment.linker_version = "14.44.35228.0";
    environment.cmake_version = "4.4.0";
    environment.generator_identity = "Ninja";
    environment.generator_version = "4.4.0";
    environment.cache_launcher_identity = "sccache";
    environment.analysis_jobs = 24u;
    environment.codegen_jobs = 12u;
    environment.host_compile_jobs_requested = 24u;
    environment.host_compile_jobs_effective = 20u;
    environment.runtime_jobs = 16u;
    environment.platform.filesystem_type = "NTFS";
    environment.platform.filesystem_quality = "os-volume-exact";
    environment.platform.storage_type = "fixed";
    environment.platform.storage_quality = "os-drive-class";
    environment.platform.energy_profile = "balanced";
    environment.platform.energy_quality =
        "os-active-scheme-exact";
    return environment;
}

} // namespace

int main() {
    const auto nonce =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    const auto root =
        std::filesystem::temp_directory_path() /
        ("katana-port-telemetry-" +
         std::to_string(nonce));
    std::error_code cleanup_error;
    std::filesystem::create_directories(root, cleanup_error);
    require(
        !cleanup_error,
        "Temporaeres Telemetrieverzeichnis konnte nicht erzeugt werden.");

    const auto disabled_path = root / "disabled.jsonl";
    {
        katana::cli::PortBuildTelemetryRecorder disabled;
        require(
            !disabled.enabled(),
            "Recorder ohne JSONL-Pfad ist nicht deaktiviert.");
        disabled.sample_resources("disabled");
        disabled.finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
    }
    require(
        !std::filesystem::exists(disabled_path),
        "Deaktivierte Telemetrie hat eine Datei erzeugt.");

    const auto blocked_parent = root / "not-a-directory";
    {
        std::ofstream file(blocked_parent, std::ios::binary);
        require(
            static_cast<bool>(file),
            "Blockierender Telemetriepfad konnte nicht vorbereitet werden.");
    }
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path =
            blocked_parent / "unwritable.jsonl";
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        const auto status = recorder.status();
        require(
            !recorder.enabled() && status.io_failed &&
                !status.terminal_emitted &&
                !status.telemetry_complete,
            "Ein explizit unbeschreibbarer Telemetriepfad blieb "
            "unbemerkt aktiv.");
    }

#ifdef _WIN32
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = root / "NUL";
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
        const auto status = recorder.status();
        const auto concrete_device_entry =
            std::find_if(
                std::filesystem::directory_iterator(root),
                std::filesystem::directory_iterator{},
                [](const auto& entry) {
                    return entry.path().filename().
                                   string().
                                   starts_with("NUL");
                });
        require(
            !recorder.enabled() && status.io_failed &&
                !status.terminal_emitted &&
                concrete_device_entry ==
                    std::filesystem::directory_iterator{},
            "Reserviertes Windows-Geraet wurde als echte "
            "Telemetriedatei akzeptiert.");
    }
#endif

    const auto owned_path = root / "writer-owned.jsonl";
    const auto owned_lock =
        katana::cli::port_build_telemetry_writer_lock_path(
            root / "." / "writer-owned.jsonl");
    require(
        owned_lock ==
            std::filesystem::absolute(
                root / "writer-owned.jsonl.katana-telemetry-writer.lock").
                lexically_normal(),
        "Oeffentlicher Writer-Lockpfad folgt nicht dem normalisierten "
        "JSONL-Ziel.");
    {
        std::ofstream stale_lock(owned_lock, std::ios::binary);
        stale_lock << "stale-lock-sentinel\n";
        require(
            static_cast<bool>(stale_lock),
            "Stale Writer-Lockdatei konnte nicht vorbereitet werden.");
    }
    {
        katana::cli::PortBuildTelemetryOptions first_options;
        first_options.jsonl_path = owned_path;
        first_options.require_resolved_environment = false;
        auto first =
            std::make_unique<
                katana::cli::PortBuildTelemetryRecorder>(
                std::move(first_options));
        require(
            first->enabled(),
            "Ownerlose persistente Writer-Lockdatei blockierte den "
            "ersten Recorder.");
        {
            katana::cli::PortBuildTelemetryOptions second_options;
            second_options.jsonl_path = owned_path;
            second_options.require_resolved_environment = false;
            katana::cli::PortBuildTelemetryRecorder second(
                std::move(second_options));
            const auto second_status = second.status();
            require(
                !second.enabled() && second_status.io_failed &&
                    !second_status.terminal_emitted,
                "Zweiter gleichzeitiger Recorder teilte denselben "
                "Writer-Lock.");
        }
        first->finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
        require(
            first->status().terminal_emitted,
            "Erster Writer-Owner konnte nicht terminal publizieren.");
        {
            katana::cli::PortBuildTelemetryOptions after_finish_options;
            after_finish_options.jsonl_path = owned_path;
            after_finish_options.require_resolved_environment = false;
            katana::cli::PortBuildTelemetryRecorder after_finish(
                std::move(after_finish_options));
            require(
                !after_finish.enabled() &&
                    after_finish.status().io_failed,
                "finish gab Writer-Ownership vor Ende der "
                "Recorder-Lebensdauer frei.");
        }
    }
    {
        katana::cli::PortBuildTelemetryOptions third_options;
        third_options.jsonl_path = owned_path;
        third_options.require_resolved_environment = false;
        katana::cli::PortBuildTelemetryRecorder third(
            std::move(third_options));
        require(
            third.enabled(),
            "Freigegebener persistenter Writer-Lock blieb nach Ende des "
            "Owners gesperrt.");
        third.finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
        require(
            third.status().terminal_emitted,
            "Dritter Recorder konnte den freigegebenen Lock nicht "
            "publizieren.");
    }
    require(
        std::filesystem::is_regular_file(owned_lock) &&
            read_lines(owned_lock) ==
                std::vector<std::string>{"stale-lock-sentinel"},
        "Persistenter Writer-Lock wurde geloescht oder inhaltlich "
        "veraendert.");

    const auto protected_alias = root / "protected-lock-alias.bin";
    {
        std::ofstream protected_file(protected_alias, std::ios::binary);
        protected_file << "protected-lock-bytes\n";
        require(
            static_cast<bool>(protected_file),
            "Geschuetzte Lock-Aliasdatei konnte nicht erzeugt werden.");
    }
    const auto hardlink_lock_target = root / "hardlink-lock.jsonl";
    const auto hardlink_lock =
        katana::cli::port_build_telemetry_writer_lock_path(
            hardlink_lock_target);
    std::error_code hardlink_lock_error;
    std::filesystem::create_hard_link(
        protected_alias,
        hardlink_lock,
        hardlink_lock_error);
    require(
        !hardlink_lock_error,
        "Hardlink fuer unsicheren Writer-Lock konnte nicht erzeugt "
        "werden.");
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = hardlink_lock_target;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            !recorder.enabled() && recorder.status().io_failed,
            "Writer-Lock als Hardlink wurde nicht fail-closed "
            "abgelehnt.");
    }
    require(
        read_lines(protected_alias) ==
            std::vector<std::string>{"protected-lock-bytes"},
        "Abgelehnter Writer-Lock-Hardlink veraenderte geschuetzte "
        "Bytes.");

    const auto directory_lock_target = root / "directory-lock.jsonl";
    const auto directory_lock =
        katana::cli::port_build_telemetry_writer_lock_path(
            directory_lock_target);
    std::filesystem::create_directory(directory_lock, cleanup_error);
    require(
        !cleanup_error,
        "Lock-Testverzeichnis konnte nicht erzeugt werden.");
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = directory_lock_target;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            !recorder.enabled() && recorder.status().io_failed,
            "Writer-Lock als Verzeichnis wurde nicht fail-closed "
            "abgelehnt.");
    }

    const auto hardlink_output_target = root / "hardlink-output.jsonl";
    std::error_code hardlink_output_error;
    std::filesystem::create_hard_link(
        protected_alias,
        hardlink_output_target,
        hardlink_output_error);
    require(
        !hardlink_output_error,
        "Hardlink fuer unsicheres JSONL-Ziel konnte nicht erzeugt "
        "werden.");
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = hardlink_output_target;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            !recorder.enabled() && recorder.status().io_failed,
            "JSONL-Ziel als Hardlink wurde nicht fail-closed "
            "abgelehnt.");
    }
    const auto directory_output_target = root / "directory-output.jsonl";
    std::filesystem::create_directory(
        directory_output_target, cleanup_error);
    require(
        !cleanup_error,
        "JSONL-Zielverzeichnis konnte nicht erzeugt werden.");
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = directory_output_target;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            !recorder.enabled() && recorder.status().io_failed,
            "JSONL-Ziel als Verzeichnis wurde nicht fail-closed "
            "abgelehnt.");
    }

    const auto symlink_output_target = root / "symlink-output.jsonl";
    const auto symlink_lock_target = root / "symlink-lock.jsonl";
    const auto symlink_lock =
        katana::cli::port_build_telemetry_writer_lock_path(
            symlink_lock_target);
    std::error_code output_symlink_error;
    std::filesystem::create_symlink(
        protected_alias,
        symlink_output_target,
        output_symlink_error);
    if (!output_symlink_error) {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = symlink_output_target;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            !recorder.enabled() && recorder.status().io_failed,
            "JSONL-Ziel als Symlink/Reparse-Point wurde nicht "
            "fail-closed abgelehnt.");
    }
    std::error_code lock_symlink_error;
    std::filesystem::create_symlink(
        protected_alias,
        symlink_lock,
        lock_symlink_error);
    if (!lock_symlink_error) {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = symlink_lock_target;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            !recorder.enabled() && recorder.status().io_failed,
            "Writer-Lock als Symlink/Reparse-Point wurde nicht "
            "fail-closed abgelehnt.");
    }

    const auto atomic_path = root / "atomic.jsonl";
    {
        std::ofstream previous(
            atomic_path, std::ios::binary);
        previous << "previous-telemetry\n";
        require(
            static_cast<bool>(previous),
            "Vorherige Telemetriedatei konnte nicht erzeugt werden.");
    }
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = atomic_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            read_lines(atomic_path) ==
                std::vector<std::string>{
                    "previous-telemetry"},
            "Recorder hat das Ziel vor dem atomaren Terminal-Publish "
            "veraendert.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
        require(
            recorder.status().terminal_emitted,
            "Atomarer Telemetrie-Publish besitzt keinen Terminalrecord.");
    }
    require(
        read_lines(atomic_path).back().find(
            "\"schema\":\"katana-port-build-terminal\"") !=
            std::string::npos,
        "Atomarer Publish hat die vorherige Datei nicht durch den "
        "vollstaendigen Stream ersetzt.");

    const auto protected_input = root / "protected-input.bin";
    const auto raced_path = root / "raced.jsonl";
    {
        std::ofstream protected_file(
            protected_input, std::ios::binary);
        protected_file << "protected-input-bytes\n";
        require(
            static_cast<bool>(protected_file),
            "Geschuetzte Race-Eingabe konnte nicht erzeugt werden.");
    }
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = raced_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        std::error_code hardlink_error;
        std::filesystem::create_hard_link(
            protected_input,
            raced_path,
            hardlink_error);
        require(
            !hardlink_error,
            "Hardlink-Race konnte nicht vorbereitet werden.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
        require(
            recorder.status().terminal_emitted,
            "Hardlink-Race verhinderte den sicheren atomaren Publish.");
    }
    require(
        read_lines(protected_input) ==
            std::vector<std::string>{
                "protected-input-bytes"} &&
            read_lines(raced_path).back().find(
                "\"schema\":\"katana-port-build-terminal\"") !=
                std::string::npos,
        "Ein nach der Initialisierung eingeschobener Hardlink hat "
        "Eingabebytes veraendert.");

#ifdef _WIN32
    const auto pinned_parent = root / "pinned-parent";
    const auto moved_parent = root / "moved-parent";
    std::filesystem::create_directories(
        pinned_parent, cleanup_error);
    require(
        !cleanup_error,
        "Parent-Pinning-Testverzeichnis konnte nicht erzeugt werden.");
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path =
            pinned_parent / "pinned.jsonl";
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.enabled(),
            "Recorder konnte das zu pinnende Parent nicht oeffnen.");
        std::error_code rename_error;
        std::filesystem::rename(
            pinned_parent, moved_parent, rename_error);
        require(
            static_cast<bool>(rename_error) &&
                std::filesystem::exists(pinned_parent) &&
                !std::filesystem::exists(moved_parent),
            "Gepinntes Telemetrie-Parent konnte waehrend des "
            "Laufs ausgetauscht werden.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
        require(
            recorder.status().terminal_emitted,
            "Parent-Pinning verhinderte den legitimen Publish.");
    }
    std::filesystem::rename(
        pinned_parent, moved_parent, cleanup_error);
    require(
        !cleanup_error &&
            std::filesystem::exists(
                moved_parent / "pinned.jsonl"),
        "Parent-/Writer-Handle blieb nach Ende der Recorder-Lebensdauer "
        "offen oder das Ergebnis fehlte.");

    const auto swap_tree = root / "swap-tree";
    const auto swap_parent = swap_tree / "parent";
    const auto moved_swap_tree = root / "swap-tree-moved";
    std::filesystem::create_directories(
        swap_parent, cleanup_error);
    require(
        !cleanup_error,
        "Ancestor-Swap-Testverzeichnis konnte nicht erzeugt werden.");
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path =
            swap_parent / "must-not-redirect.jsonl";
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.enabled(),
            "Recorder konnte das Ancestor-Swap-Parent nicht pinnen.");
        std::error_code ancestor_rename_error;
        std::filesystem::rename(
            swap_tree,
            moved_swap_tree,
            ancestor_rename_error);
        if (!ancestor_rename_error) {
            std::filesystem::create_directories(
                swap_parent, cleanup_error);
            require(
                !cleanup_error,
                "Ersatzbaum fuer den Ancestor-Swap konnte nicht "
                "erzeugt werden.");
            const auto replacement_sentinel =
                swap_parent / "sentinel.bin";
            {
                std::ofstream sentinel(
                    replacement_sentinel,
                    std::ios::binary);
                sentinel << "replacement-untouched\n";
                require(
                    static_cast<bool>(sentinel),
                    "Ancestor-Swap-Sentinel konnte nicht erzeugt "
                    "werden.");
            }
            recorder.finish(
                katana::cli::PortBuildTerminalOutcome::Completed);
            const auto status = recorder.status();
            require(
                !status.terminal_emitted && status.io_failed &&
                    !std::filesystem::exists(
                        swap_parent /
                        "must-not-redirect.jsonl") &&
                    read_lines(replacement_sentinel) ==
                        std::vector<std::string>{
                            "replacement-untouched"},
                "Ancestor-Swap wurde auf den Ersatzbaum umgelenkt "
                "oder nicht fail-closed gemeldet.");
        } else {
            recorder.finish(
                katana::cli::PortBuildTerminalOutcome::Completed);
            require(
                recorder.status().terminal_emitted,
                "Vom Betriebssystem blockierter Ancestor-Swap "
                "verhinderte den legitimen Publish.");
        }
    }

    const auto symlink_target = root / "symlink-target";
    const auto symlink_parent = root / "symlink-parent";
    std::filesystem::create_directories(
        symlink_target, cleanup_error);
    require(
        !cleanup_error,
        "Symlink-Zielverzeichnis konnte nicht erzeugt werden.");
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(
        symlink_target, symlink_parent, symlink_error);
    if (!symlink_error) {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path =
            symlink_parent / "redirected.jsonl";
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        const auto status = recorder.status();
        require(
            !recorder.enabled() && status.io_failed &&
                !std::filesystem::exists(
                    symlink_target / "redirected.jsonl"),
            "Symlink als Telemetrie-Parent wurde nicht fail-closed "
            "abgelehnt.");
    }
#endif

    const auto configured_build = root / "configured-build";
    std::filesystem::create_directories(
        configured_build / "CMakeFiles" / "4.4.0");
    {
        std::ofstream cache(
            configured_build / "CMakeCache.txt",
            std::ios::binary);
        cache
            << "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4\n"
            << "CMAKE_CACHE_MINOR_VERSION:INTERNAL=4\n"
            << "CMAKE_CACHE_PATCH_VERSION:INTERNAL=0\n"
            << "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            << "CMAKE_CXX_COMPILER_LAUNCHER:STRING="
            << (root / "private-user" / "sccache.exe").string()
            << "\n";
        require(
            static_cast<bool>(cache),
            "CMakeCache-Testzustand konnte nicht geschrieben werden.");
    }
    {
        std::ofstream compiler(
            configured_build / "CMakeFiles" / "4.4.0" /
                "CMakeCXXCompiler.cmake",
            std::ios::binary);
        compiler
            << "set(CMAKE_CXX_COMPILER_ID \"MSVC\")\n"
            << "set(CMAKE_CXX_COMPILER_VERSION \"19.44.35228.0\")\n"
            << "set(CMAKE_CXX_COMPILER_LINKER_ID \"MSVC\")\n"
            << "set(CMAKE_CXX_COMPILER_LINKER_VERSION "
               "\"14.44.35228.0\")\n";
        require(
            static_cast<bool>(compiler),
            "CMake-Compiler-Testzustand konnte nicht geschrieben werden.");
    }
    const auto parsed_environment =
        katana::cli::resolve_port_build_cmake_environment(
            configured_build,
            24u,
            0u,
            24u,
            20u,
            16u);
    require(
        parsed_environment.compiler_identity == "MSVC" &&
            parsed_environment.compiler_version ==
                "19.44.35228.0" &&
            parsed_environment.linker_identity == "MSVC" &&
            parsed_environment.linker_version ==
                "14.44.35228.0" &&
            parsed_environment.cmake_version == "4.4.0" &&
            parsed_environment.generator_identity == "Ninja" &&
            parsed_environment.generator_version == "4.4.0" &&
            parsed_environment.cache_launcher_identity == "sccache" &&
            parsed_environment.analysis_jobs == 24u &&
            parsed_environment.codegen_jobs == 0u &&
            parsed_environment.host_compile_jobs_requested == 24u &&
            parsed_environment.host_compile_jobs_effective == 20u &&
            parsed_environment.runtime_jobs == 16u,
        "CMake-Zustand wurde nicht als echte Child-Umgebung gebunden.");

    const auto legacy_build = root / "legacy-configured-build";
    const auto legacy_linker = root / "private-user" / "ld-test";
    std::filesystem::create_directories(
        legacy_build / "CMakeFiles" / "3.25.3");
    std::filesystem::create_directories(
        legacy_linker.parent_path());
    {
        std::ofstream cache(
            legacy_build / "CMakeCache.txt",
            std::ios::binary);
        cache
            << "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=3\n"
            << "CMAKE_CACHE_MINOR_VERSION:INTERNAL=25\n"
            << "CMAKE_CACHE_PATCH_VERSION:INTERNAL=3\n"
            << "CMAKE_GENERATOR:INTERNAL=Ninja\n";
        std::ofstream linker(legacy_linker, std::ios::binary);
        linker << "synthetic-linker-binary";
        require(
            static_cast<bool>(cache) &&
                static_cast<bool>(linker),
            "Legacy-CMake-Testzustand konnte nicht geschrieben werden.");
    }
    {
        std::ofstream compiler(
            legacy_build / "CMakeFiles" / "3.25.3" /
                "CMakeCXXCompiler.cmake",
            std::ios::binary);
        compiler
            << "set(CMAKE_CXX_COMPILER_ID \"GNU\")\n"
            << "set(CMAKE_CXX_COMPILER_VERSION \"12.2.0\")\n"
            << "set(CMAKE_LINKER \""
            << legacy_linker.string()
            << "\")\n";
        require(
            static_cast<bool>(compiler),
            "Legacy-CMake-Compilerzustand konnte nicht geschrieben werden.");
    }
    const auto legacy_environment =
        katana::cli::resolve_port_build_cmake_environment(
            legacy_build,
            8u,
            2u,
            8u,
            8u,
            4u);
    require(
        legacy_environment.linker_identity == "ld-test" &&
            legacy_environment.linker_version.starts_with(
                "sha256:") &&
            legacy_environment.linker_version.size() == 71u &&
            legacy_environment.linker_quality ==
                "binary-content-identity",
        "Aelterer CMake-Zustand bindet den echten Linker nicht ueber "
        "seine Binaeridentitaet.");

    const auto complete_path = root / "complete.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = complete_path;
        options.build_profile = "Release";
        options.job_kind = "native-disc";
        options.require_phase_timings = true;
        options.gpu_identity =
            "C:\\private-user\\gpu-name";
        options.resource_sample_interval =
            std::chrono::milliseconds(100);
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.enabled(),
            "Recorder mit gueltigem JSONL-Pfad ist nicht aktiv.");
        require(
            recorder.record_resolved_environment(
                resolved_environment()),
            "Gueltige post-Configure-Umgebung wurde nicht gebunden.");
        require(
            recorder.record_host_command(
                katana::cli::PortBuildHostCommandObservation{
                    "configure",
                    0,
                    false,
                    false,
                    std::nullopt,
                    true,
                    "job-object-tree",
                    true}),
            "Gueltiger kritischer Hostprozessrecord wurde nicht "
            "angenommen.");

        katana::ProgressEvent started;
        started.operation =
            katana::ProgressOperation::PortBuild;
        started.state = katana::ProgressState::Started;
        started.unit = katana::ProgressUnit::Steps;
        started.scope_id = 7u;
        started.total = 1u;
        started.label = "native-disc";
        recorder.observe_progress(started);
        recorder.sample_resources("analysis");

        auto completed = started;
        completed.state = katana::ProgressState::Completed;
        completed.sequence = 2u;
        completed.completed = 1u;
        completed.label = "callsite-0x8C053C66";
        recorder.observe_progress(completed);
        const std::array timing_samples{
            katana::cli::PortBuildPhaseTimingSample{
                "analysis", 17u, false},
            katana::cli::PortBuildPhaseTimingSample{
                "export:latent-aot-module-analysis-2", 5u, true}};
        require(
            recorder.record_phase_timings(23u, timing_samples),
            "Versionierter Phase-Timing-Record wurde nicht angenommen.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed,
            0,
            "packaging");
        const auto status = recorder.status();
        require(
            status.terminal_emitted &&
                status.telemetry_complete &&
                !status.io_failed &&
                status.lost_records == 0u,
            "Vollstaendiger Lauf besitzt keinen vollstaendigen Terminalstatus.");
    }

    const auto complete_lines = read_lines(complete_path);
    require(
        complete_lines.size() >= 7u,
        "Manifest, Umgebung, Progress, Ressourcen und Terminal fehlen.");
    require_gapless_sequence(complete_lines);
    require(
        complete_lines.front().find(
            "\"schema\":\"katana-port-build-manifest\"") !=
            std::string::npos &&
            complete_lines.back().find(
                "\"schema\":\"katana-port-build-terminal\"") !=
                std::string::npos &&
            complete_lines.back().find(
                "\"telemetry_complete\":true") !=
                std::string::npos,
        "Versionierte Manifest-/Terminalschemas fehlen.");
    const auto resolved = std::find_if(
        complete_lines.begin(),
        complete_lines.end(),
        [](const auto& line) {
            return line.find(
                       "\"schema\":\"katana-port-build-resolved-environment\"") !=
                   std::string::npos;
        });
    const auto host_command = std::find_if(
        complete_lines.begin(),
        complete_lines.end(),
        [](const auto& line) {
            return line.find(
                       "\"schema\":\"katana-port-build-host-command\"") !=
                   std::string::npos;
        });
    const auto phase_timings = std::find_if(
        complete_lines.begin(),
        complete_lines.end(),
        [](const auto& line) {
            return line.find(
                       "\"schema\":\"katana-port-phase-timings\"") !=
                   std::string::npos;
        });
    const auto resource = std::find_if(
        complete_lines.begin(),
        complete_lines.end(),
        [](const auto& line) {
            return line.find(
                       "\"schema\":\"katana-port-build-resource\"") !=
                   std::string::npos;
        });
    require(
        complete_lines.front().find(
            "\"telemetry_binary\":{\"role\":\"observer\"") !=
                std::string::npos &&
            complete_lines.front().find(
                "\"toolchain\"") == std::string::npos &&
            complete_lines.front().find(
                "\"requested_environment\"") !=
                std::string::npos &&
            complete_lines.front().find(
                "\"numeric_value\"") == std::string::npos &&
            complete_lines.front().find(
                "\"raw_value_omitted\":true") !=
                std::string::npos &&
            resolved != complete_lines.end() &&
            resolved->find(
                "\"compiler\":{\"identity\":\"MSVC\",\"version\":\"19.44.35228.0\"") !=
                std::string::npos &&
            resolved->find(
                "\"linker\":{\"identity\":\"MSVC\",\"version\":\"14.44.35228.0\"") !=
                std::string::npos &&
            resolved->find(
                "\"cmake\":{\"version\":\"4.4.0\"}") !=
                std::string::npos &&
            resolved->find(
                "\"generator\":{\"identity\":\"Ninja\",\"version\":\"4.4.0\"") !=
                std::string::npos &&
            resolved->find(
                "\"jobs\":{\"analysis\":24,\"codegen\":12,"
                "\"host_compile_requested\":24,"
                "\"host_compile_effective\":20,"
                "\"host_build\":20,"
                "\"runtime_parallel_work\":16") !=
                std::string::npos &&
            resolved->find(
                "\"cache_launcher\":{\"identity\":\"sccache\",\"enabled\":true") !=
                std::string::npos &&
            resolved->find(
                "\"filesystem\":{\"type\":\"NTFS\",\"quality\":\"os-volume-exact\"}") !=
                std::string::npos &&
            resolved->find(
                "\"energy\":{\"profile\":\"balanced\",\"quality\":\"os-active-scheme-exact\"}") !=
                std::string::npos &&
            host_command != complete_lines.end() &&
            host_command->find(
                "\"stage\":\"configure\",\"host_exit_code\":0,\"timed_out\":false") !=
                std::string::npos &&
            host_command->find(
                "\"process_tree_quiescent\":true,"
                "\"process_tree_scope\":\"job-object-tree\","
                "\"process_tree_query_complete\":true") !=
                std::string::npos &&
            phase_timings != complete_lines.end() &&
            phase_timings->find(
                "\"total_ms\":23,\"samples\":[{\"phase\":\"analysis\","
                "\"duration_ms\":17,\"parallel\":false}") !=
                std::string::npos &&
            phase_timings->find(
                "\"phase\":\"export:latent-aot-module-analysis-2\","
                "\"duration_ms\":5,\"parallel\":true") !=
                std::string::npos &&
            resource != complete_lines.end() &&
            resource->find("\"cpu_quality\":\"") !=
                std::string::npos &&
            resource->find("\"memory_quality\":\"") !=
                std::string::npos &&
            resource->find("\"faults_quality\":\"") !=
                std::string::npos &&
            resource->find("\"processes_quality\":\"") !=
                std::string::npos,
        "Resolved-Environment bindet Toolchain, Generator, Jobs, Cache "
        "oder Plattform nicht exakt.");
    for (const auto& line : complete_lines) {
        require(
            line.find("private-user") == std::string::npos &&
                line.find("toolchain.exe") ==
                    std::string::npos &&
                line.find("0x8C053C66") ==
                    std::string::npos,
            "Telemetrie leakt einen privaten Pfad oder eine Gastadresse.");
    }

    const auto inspected_platform =
        katana::cli::inspect_port_build_resolved_platform(root);
    for (const auto* value : {
             &inspected_platform.filesystem_type,
             &inspected_platform.filesystem_quality,
             &inspected_platform.storage_type,
             &inspected_platform.storage_quality,
             &inspected_platform.energy_profile,
             &inspected_platform.energy_quality}) {
        require(
            !value->empty() &&
                value->find('/') == std::string::npos &&
                value->find('\\') == std::string::npos,
            "Plattformprobe gab einen leeren Wert oder privaten Pfad aus.");
    }

    const auto missing_environment_path =
        root / "missing-environment.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = missing_environment_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed);
        const auto status = recorder.status();
        require(
            status.terminal_emitted &&
                !status.telemetry_complete &&
                !status.resolved_environment_recorded,
            "Explizite Telemetrie ohne kritischen Umgebungsrecord "
            "wurde als vollstaendig gemeldet.");
    }
    const auto missing_environment_terminal =
        read_lines(missing_environment_path).back();
    require(
        missing_environment_terminal.find(
            "\"outcome\":\"failed\"") != std::string::npos &&
            missing_environment_terminal.find(
                "\"exit_code\":4") != std::string::npos &&
            missing_environment_terminal.find(
                "\"telemetry_complete\":false") !=
                std::string::npos,
        "Terminalrecord stuft einen unvollstaendigen Completed-Abschluss "
        "nicht vor der Publikation auf Failed/InputOutput herab.");

    const auto host_timeout_path = root / "host-timeout.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = host_timeout_path;
        options.require_phase_timings = true;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.record_resolved_environment(
                resolved_environment()),
            "Timeout-Test konnte die Umgebung nicht binden.");
        require(
            recorder.record_host_command(
                katana::cli::PortBuildHostCommandObservation{
                    "host-build",
                    124,
                    true,
                    false,
                    std::nullopt,
                    true,
                    "job-object-tree",
                    true}),
            "Hosttimeout wurde nicht separat aufgezeichnet.");
        const std::array timing_samples{
            katana::cli::PortBuildPhaseTimingSample{
                "host-build", 17u, false}};
        require(
            recorder.record_phase_timings(17u, timing_samples),
            "Hostfehler-Timingrecord wurde nicht angenommen.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Failed,
            7,
            "host-build");
    }
    const auto host_timeout_lines = read_lines(host_timeout_path);
    const auto host_timeout = std::find_if(
        host_timeout_lines.begin(),
        host_timeout_lines.end(),
        [](const auto& line) {
            return line.find(
                       "\"schema\":\"katana-port-build-host-command\"") !=
                   std::string::npos;
        });
    const auto host_timeout_timing = std::find_if(
        host_timeout_lines.begin(),
        host_timeout_lines.end(),
        [](const auto& line) {
            return line.find(
                       "\"schema\":\"katana-port-phase-timings\"") !=
                   std::string::npos;
        });
    require(
        host_timeout != host_timeout_lines.end() &&
            host_timeout_timing != host_timeout_lines.end() &&
            host_timeout_timing < host_timeout_lines.end() - 1 &&
            host_timeout->find(
                "\"host_exit_code\":124,\"timed_out\":true") !=
                std::string::npos &&
            host_timeout_lines.back().find(
                "\"schema\":\"katana-port-build-terminal\"") !=
                std::string::npos &&
            host_timeout_lines.back().find(
                "\"exit_code\":7") != std::string::npos &&
            host_timeout_lines.back().find(
                "\"phase\":\"host-build\"") !=
                std::string::npos &&
            host_timeout_lines.back().find(
                "\"exit_code\":124") == std::string::npos,
        "Hosttimeout und tatsaechlicher CLI-BuildFailure-Exit wurden "
        "im Terminalrecord vermischt.");

    const auto invalid_environment_path =
        root / "invalid-environment.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = invalid_environment_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        auto environment = resolved_environment();
        environment.compiler_identity =
            "C:\\private-user\\child-cl.exe";
        require(
            !recorder.record_resolved_environment(
                std::move(environment)),
            "Privater Toolchainpfad wurde als aufgeloeste Identitaet "
            "akzeptiert.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Failed);
        require(
            !recorder.status().telemetry_complete,
            "Abgelehnte Umgebungsbindung blieb vollstaendig.");
    }
    for (const auto& line : read_lines(invalid_environment_path)) {
        require(
            line.find("private-user") == std::string::npos &&
                line.find("child-cl.exe") == std::string::npos,
            "Abgelehnte Toolchainidentitaet wurde trotzdem geschrieben.");
    }

    const auto admission_path = root / "queue-admission.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = admission_path;
        options.maximum_record_bytes = 4u * 1'024u;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        auto oversized = resolved_environment();
        const std::string escaped_label(160u, '"');
        oversized.compiler_identity = escaped_label;
        oversized.compiler_version = escaped_label;
        oversized.compiler_quality = escaped_label;
        oversized.linker_identity = escaped_label;
        oversized.linker_version = escaped_label;
        oversized.linker_quality = escaped_label;
        oversized.cmake_version = escaped_label;
        oversized.generator_identity = escaped_label;
        oversized.generator_version = escaped_label;
        oversized.generator_version_quality = escaped_label;
        oversized.cache_launcher_identity = escaped_label;
        oversized.cache_launcher_quality = escaped_label;
        oversized.platform.filesystem_type = escaped_label;
        oversized.platform.filesystem_quality = escaped_label;
        oversized.platform.storage_type = escaped_label;
        oversized.platform.storage_quality = escaped_label;
        oversized.platform.energy_profile = escaped_label;
        oversized.platform.energy_quality = escaped_label;
        require(
            !recorder.record_resolved_environment(
                std::move(oversized)) &&
                !recorder.status().resolved_environment_recorded,
            "Zu grosser Umgebungsrecord meldete eine Queue-Aufnahme.");
        require(
            recorder.record_resolved_environment(
                resolved_environment()) &&
                recorder.status().resolved_environment_recorded,
            "Abgelehnte Queue-Aufnahme blockierte die gueltige "
            "Umgebungsbindung dauerhaft.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Failed);
    }

    const auto saturated_path = root / "queue-saturated.jsonl";
    constexpr std::size_t saturation_workers = 16u;
    constexpr std::size_t saturation_attempts_per_worker = 128u;
    std::atomic_size_t saturation_ready = 0u;
    std::atomic_bool saturation_start = false;
    std::atomic_size_t saturation_accepted = 0u;
    std::atomic_size_t saturation_rejected = 0u;
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = saturated_path;
        options.maximum_pending_records = 8u;
        options.require_resolved_environment = false;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        std::array<std::jthread, saturation_workers> producers;
        for (auto& producer : producers) {
            producer = std::jthread([&] {
                saturation_ready.fetch_add(
                    1u, std::memory_order_release);
                while (!saturation_start.load(
                    std::memory_order_acquire))
                    std::this_thread::yield();
                for (std::size_t attempt = 0u;
                     attempt < saturation_attempts_per_worker;
                     ++attempt) {
                    const auto accepted = recorder.record_host_command(
                        katana::cli::PortBuildHostCommandObservation{
                            "host-build",
                            0,
                            false,
                            false,
                            std::nullopt,
                            true,
                            "job-object-tree",
                            true});
                    (accepted ? saturation_accepted
                              : saturation_rejected)
                        .fetch_add(1u, std::memory_order_relaxed);
                }
            });
        }
        while (saturation_ready.load(
                   std::memory_order_acquire) !=
               saturation_workers)
            std::this_thread::yield();
        saturation_start.store(true, std::memory_order_release);
        for (auto& producer : producers)
            producer.join();
        require(
            saturation_rejected.load(
                std::memory_order_relaxed) != 0u &&
                recorder.status().lost_records != 0u,
            "Kleine kritische Queue meldete keine echte Saettigung.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Failed);
    }
    const auto saturated_lines = read_lines(saturated_path);
    const auto admitted_host_records = static_cast<std::size_t>(
        std::count_if(
            saturated_lines.begin(),
            saturated_lines.end(),
            [](const auto& line) {
                return line.find(
                           "\"schema\":\"katana-port-build-host-command\"") !=
                       std::string::npos;
            }));
    require(
        admitted_host_records == saturation_accepted.load(
                                     std::memory_order_relaxed) &&
            saturation_accepted.load(std::memory_order_relaxed) +
                    saturation_rejected.load(std::memory_order_relaxed) ==
                saturation_workers * saturation_attempts_per_worker,
        "Hostkommando-Rueckgabewert entspricht nicht der echten "
        "Queue-Aufnahme.");

    const auto incomplete_path = root / "incomplete.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = incomplete_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        katana::ProgressEvent event;
        event.operation =
            katana::ProgressOperation::ControlFlowAnalysis;
        event.state = katana::ProgressState::Running;
        event.scope_id = 9u;
        event.dropped_observations = 3u;
        event.telemetry_complete = false;
        recorder.observe_progress(event);
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Failed,
            17,
            "analysis");
        const auto status = recorder.status();
        require(
            status.terminal_emitted &&
                !status.telemetry_complete &&
                status.upstream_dropped_observations == 3u,
            "Upstreamverlust erreicht den Terminalvertrag nicht.");
    }
    const auto incomplete_lines =
        read_lines(incomplete_path);
    require_gapless_sequence(incomplete_lines);
    require(
        incomplete_lines.back().find(
            "\"outcome\":\"failed\"") !=
                std::string::npos &&
            incomplete_lines.back().find(
                "\"upstream_dropped_observations\":3") !=
                std::string::npos &&
            incomplete_lines.back().find(
                "\"telemetry_complete\":false") !=
            std::string::npos,
        "Terminalrecord verschweigt Upstreamverlust oder Fehlerausgang.");

    const auto invalid_incremental_path =
        root / "invalid-incremental-ledger.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = invalid_incremental_path;
        options.require_resolved_environment = false;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        katana::ProgressEvent event;
        event.operation =
            katana::ProgressOperation::FunctionValueAnalysis;
        event.state = katana::ProgressState::Completed;
        event.scope_id = 10u;
        event.telemetry_complete = true;
        event.counters.analysis_epochs_published = 1u;
        event.counters.analysis_epochs_discarded = 0u;
        event.counters.incremental_epochs_started = 1u;
        event.counters.resolution_root_artifacts_total = 2u;
        event.counters.resolution_root_artifacts_reused = 2u;
        event.counters.resolution_root_artifacts_recomputed = 1u;
        event.counters.resolution_root_artifacts_retained = 2u;
        event.counters.resolution_epoch_retained_bytes = 2'048u;
        event.counters.resolution_retention_limit_reason = "none";
        event.counters.persistent_analysis_bypass_reason = "none";
        event.counters.dirty_sccs = 1u;
        event.counters.dirty_functions = 1u;
        event.counters.dirty_inventory_sinks = 0u;
        event.counters.full_cpu_recompute_fallbacks = 0u;
        recorder.observe_progress(event);
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed,
            0,
            "complete");
        require(
            recorder.status().terminal_emitted &&
                !recorder.status().telemetry_complete,
            "Der Port-Recorder akzeptierte ein unausgeglichenes "
            "Inkremental-Ledger als vollstaendig.");
    }
    const auto invalid_incremental_lines =
        read_lines(invalid_incremental_path);
    const auto invalid_incremental_progress = std::find_if(
        invalid_incremental_lines.begin(),
        invalid_incremental_lines.end(),
        [](const auto& line) {
            return line.find(
                       "\"schema\":\"katana-port-build-progress\"") !=
                   std::string::npos;
        });
    require(
        invalid_incremental_progress !=
                invalid_incremental_lines.end() &&
            invalid_incremental_progress->find(
                "\"progress\":{") != std::string::npos &&
            invalid_incremental_progress->find(
                "\"telemetry_complete\":false") !=
                std::string::npos &&
            invalid_incremental_lines.back().find(
                "\"upstream_incomplete\":true") !=
                std::string::npos &&
            invalid_incremental_lines.back().find(
                "\"telemetry_complete\":false") !=
                std::string::npos,
        "Eingebettetes oder aeusseres Port-Telemetrieledger blieb trotz "
        "Root-Ungleichheit gruen.");

    const auto callback_seal_path = root / "callback-seal-loss.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = callback_seal_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.record_resolved_environment(
                resolved_environment()),
            "Callback-Seal-Test konnte die Umgebung nicht binden.");
        const katana::ProgressReporter reporter(
            [&](const katana::ProgressEvent& event) {
                recorder.observe_progress(event);
                if (event.state == katana::ProgressState::Completed)
                    throw std::runtime_error(
                        "deterministic-post-observation-loss");
            },
            std::chrono::milliseconds(0));
        auto scope = reporter.begin(
            katana::ProgressOperation::PortBuild,
            katana::ProgressUnit::Steps,
            1u,
            "callback-seal-loss");
        scope.complete();
        const auto sealed = reporter.seal_and_flush();
        require(
            !sealed && reporter.dropped_observations() == 1u,
            "Geworfener Callback wurde vom Progress-Seal nicht erkannt.");
        require(
            recorder.mark_upstream_incomplete(
                "progress-seal-failed",
                reporter.dropped_observations()),
            "Callback-Seal-Verlust wurde nicht vor Closing gebunden.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed,
            0,
            "complete");
    }
    const auto callback_seal_terminal =
        read_lines(callback_seal_path).back();
    require(
        callback_seal_terminal.find(
            "\"outcome\":\"failed\"") != std::string::npos &&
            callback_seal_terminal.find(
                "\"upstream_incomplete\":true") !=
                std::string::npos &&
            callback_seal_terminal.find(
                "\"upstream_incomplete_reason\":\"progress-seal-failed\"") !=
                std::string::npos &&
            callback_seal_terminal.find(
                "\"telemetry_complete\":false") !=
                std::string::npos,
        "Nach Completed beobachteter Callbackverlust blieb im Terminal "
        "faelschlich vollstaendig.");

    const auto active_scope_seal_path = root / "active-scope-seal-loss.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = active_scope_seal_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.record_resolved_environment(
                resolved_environment()),
            "Active-Scope-Seal-Test konnte die Umgebung nicht binden.");
        const katana::ProgressReporter reporter(
            recorder.progress_callback(),
            std::chrono::milliseconds(0));
        auto active_scope = reporter.begin(
            katana::ProgressOperation::PortBuild,
            katana::ProgressUnit::Steps,
            1u,
            "still-active-at-seal");
        const auto sealed = reporter.seal_and_flush();
        require(
            !sealed && reporter.dropped_observations() == 0u,
            "Aktiver Scope wurde vom Progress-Seal nicht erkannt.");
        require(
            recorder.mark_upstream_incomplete(
                "progress-seal-failed",
                reporter.dropped_observations()),
            "Active-Scope-Seal-Verlust wurde nicht vor Closing gebunden.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed,
            0,
            "complete");
    }
    const auto active_scope_seal_terminal =
        read_lines(active_scope_seal_path).back();
    require(
        active_scope_seal_terminal.find(
            "\"outcome\":\"failed\"") != std::string::npos &&
            active_scope_seal_terminal.find(
                "\"upstream_incomplete\":true") !=
                std::string::npos &&
            active_scope_seal_terminal.find(
                "\"upstream_incomplete_reason\":\"progress-seal-failed\"") !=
                std::string::npos &&
            active_scope_seal_terminal.find(
                "\"telemetry_complete\":false") !=
                std::string::npos,
        "Aktiver Scope beim Seal blieb im Terminal faelschlich "
        "vollstaendig.");

    const auto upstream_count_path = root / "upstream-count-race.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = upstream_count_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.record_resolved_environment(
                resolved_environment()),
            "Upstream-Zaehler-Test konnte die Umgebung nicht binden.");
        std::array<std::jthread, 4u> event_maximizers;
        for (std::size_t worker = 0u;
             worker < event_maximizers.size(); ++worker) {
            event_maximizers[worker] = std::jthread([&, worker] {
                for (std::uint64_t local = 0u; local < 100u; ++local) {
                    const auto index =
                        static_cast<std::uint64_t>(worker) * 100u + local;
                    katana::ProgressEvent event;
                    event.operation =
                        katana::ProgressOperation::PortBuild;
                    event.state = katana::ProgressState::Running;
                    event.scope_id = index + 1u;
                    event.dropped_observations = 1'000'000u + index;
                    event.telemetry_complete = false;
                    recorder.observe_progress(event);
                }
            });
        }
        std::jthread seal_bridge([&] {
            for (std::uint64_t index = 0u; index < 100u; ++index)
                static_cast<void>(
                    recorder.mark_upstream_incomplete(
                        "progress-seal-failed", index + 1u));
        });
        for (auto& event_maximizer : event_maximizers)
            event_maximizer.join();
        seal_bridge.join();
        require(
            recorder.status().upstream_dropped_observations ==
                1'000'399u,
            "Event-Maximum und Seal-Bridge-Delta ueberschreiben sich "
            "nebenlaeufig.");
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Failed,
            4,
            "progress-seal");
    }
    require_gapless_sequence(read_lines(upstream_count_path));

    const auto bridge_finish_path = root / "bridge-finish-fence.jsonl";
    std::size_t admitted_bridge_count = 0u;
    for (std::size_t iteration = 0u; iteration < 64u; ++iteration) {
        std::atomic_bool bridge_started = false;
        std::atomic_bool bridge_retained = false;
        {
            katana::cli::PortBuildTelemetryOptions options;
            options.jsonl_path = bridge_finish_path;
            options.require_resolved_environment = false;
            katana::cli::PortBuildTelemetryRecorder recorder(
                std::move(options));
            std::jthread bridge([&] {
                bridge_started.store(
                    true, std::memory_order_release);
                bridge_retained.store(
                    recorder.mark_upstream_incomplete(
                        "progress-seal-failed", 1u),
                    std::memory_order_release);
            });
            while (!bridge_started.load(
                std::memory_order_acquire))
                std::this_thread::yield();
            recorder.finish(
                katana::cli::PortBuildTerminalOutcome::Completed,
                0,
                "complete");
            bridge.join();
        }
        const auto terminal = read_lines(bridge_finish_path).back();
        const auto completed = terminal.find(
            "\"outcome\":\"completed\"") != std::string::npos;
        const auto incomplete = terminal.find(
            "\"upstream_incomplete\":true") != std::string::npos;
        require(
            !(completed && incomplete),
            "Concurrent Bridge/finish publizierte Completed mit "
            "unvollstaendigem Stream.");
        if (bridge_retained.load(std::memory_order_acquire)) {
            ++admitted_bridge_count;
            require(
                !completed && incomplete &&
                    terminal.find(
                        "\"telemetry_complete\":false") !=
                        std::string::npos,
                "Vor Closing zugelassene Bridge lag nicht im "
                "Terminal-Fence.");
        }
    }
    require(
        admitted_bridge_count != 0u,
        "Concurrent Bridge/finish-Test liess keine Bridge vor Closing zu.");

    const auto concurrent_path = root / "concurrent.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = concurrent_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.record_resolved_environment(
                resolved_environment()),
            "Concurrent-Test konnte die Umgebung nicht binden.");
        std::array<std::thread, 8u> finishers;
        for (auto& finisher : finishers) {
            finisher = std::thread([&] {
                recorder.finish(
                    katana::cli::PortBuildTerminalOutcome::Completed,
                    0,
                    "concurrent");
            });
        }
        for (auto& finisher : finishers)
            finisher.join();
        const auto status = recorder.status();
        require(
            status.terminal_emitted &&
                status.telemetry_complete,
            "Nebenlaeufige idempotente Terminalisierung ist nicht sicher.");
    }
    const auto concurrent_lines =
        read_lines(concurrent_path);
    require_gapless_sequence(concurrent_lines);
    std::size_t terminal_count = 0u;
    for (const auto& line : concurrent_lines) {
        if (line.find(
                "\"schema\":\"katana-port-build-terminal\"") !=
            std::string::npos)
            ++terminal_count;
    }
    require(
        terminal_count == 1u,
        "Nebenlaeufiges finish schreibt mehr als einen Terminalrecord.");

    const auto drained_path = root / "drained.jsonl";
    {
        katana::cli::PortBuildTelemetryOptions options;
        options.jsonl_path = drained_path;
        katana::cli::PortBuildTelemetryRecorder recorder(
            std::move(options));
        require(
            recorder.record_resolved_environment(
                resolved_environment()),
            "Drain-Test konnte die Umgebung nicht binden.");
        std::atomic_bool callback_entered = false;
        std::atomic_bool callback_release = false;
        std::atomic_bool flush_completed = false;
        const katana::ProgressReporter reporter(
            [&](const katana::ProgressEvent& event) {
                recorder.observe_progress(event);
                if (event.state ==
                    katana::ProgressState::Started) {
                    callback_entered.store(
                        true, std::memory_order_release);
                    while (!callback_release.load(
                        std::memory_order_acquire))
                        std::this_thread::yield();
                }
            },
            std::chrono::milliseconds(0));
        std::jthread producer([&] {
            auto scope = reporter.begin(
                katana::ProgressOperation::PortBuild,
                katana::ProgressUnit::Steps,
                1u,
                "drain-order");
            scope.complete();
        });
        const auto callback_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (!callback_entered.load(
                   std::memory_order_acquire) &&
               std::chrono::steady_clock::now() <
                   callback_deadline)
            std::this_thread::yield();
        require(
            callback_entered.load(
                std::memory_order_acquire),
            "Drain-Test erreichte den blockierten Callback nicht.");
        std::jthread flusher([&] {
            flush_completed.store(
                reporter.flush(),
                std::memory_order_release);
        });
        std::this_thread::sleep_for(
            std::chrono::milliseconds(25));
        require(
            !flush_completed.load(
                std::memory_order_acquire),
            "Progress-Flush kehrte vor dem laufenden Callback zurueck.");
        callback_release.store(
            true, std::memory_order_release);
        producer.join();
        flusher.join();
        recorder.finish(
            katana::cli::PortBuildTerminalOutcome::Completed,
            0,
            "drained");
    }
    const auto drained_lines = read_lines(drained_path);
    require_gapless_sequence(drained_lines);
    const auto drained_started = std::find_if(
        drained_lines.begin(),
        drained_lines.end(),
        [](const auto& line) {
            return line.find("\"state\":\"started\"") !=
                   std::string::npos;
        });
    const auto drained_completed = std::find_if(
        drained_lines.begin(),
        drained_lines.end(),
        [](const auto& line) {
            return line.find("\"state\":\"completed\"") !=
                   std::string::npos;
        });
    require(
        drained_lines.size() >= 4u &&
            drained_started != drained_lines.end() &&
            drained_completed != drained_lines.end() &&
            drained_started < drained_completed &&
            drained_completed <
                std::prev(drained_lines.end()) &&
            drained_lines.back().find(
                "\"schema\":\"katana-port-build-terminal\"") !=
                std::string::npos &&
            drained_lines.back().find(
                "\"telemetry_complete\":true") !=
                std::string::npos,
        "Progress-Drain verlor Started/Completed oder schrieb sie "
        "hinter den Telemetrieabschluss.");

    std::filesystem::remove_all(root, cleanup_error);
    std::cout
        << "Portbuild-Telemetrie-Regressionen erfolgreich.\n";
    return EXIT_SUCCESS;
}
