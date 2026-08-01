#include "host_build_progress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("katana-host-build-progress-" +
                 std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

struct EventLog final {
    std::mutex mutex;
    std::vector<katana::ProgressEvent> events;

    void append(const katana::ProgressEvent& event) {
        const std::scoped_lock lock(mutex);
        events.push_back(event);
    }

    [[nodiscard]] std::vector<katana::ProgressEvent> snapshot() {
        const std::scoped_lock lock(mutex);
        return events;
    }
};

const katana::ProgressEvent& require_terminal(
    const std::vector<katana::ProgressEvent>& events,
    const std::string_view label) {
    const auto found = std::find_if(
        events.rbegin(), events.rend(),
        [&](const auto& event) {
            return event.label == label &&
                   (event.state == katana::ProgressState::Completed ||
                    event.state == katana::ProgressState::Cached ||
                    event.state == katana::ProgressState::Failed);
        });
    if (found == events.rend())
        throw std::runtime_error(
            "Terminales Hostbuild-Ereignis fehlt: " +
            std::string(label));
    return *found;
}

katana::cli::HostBuildCompletionProof completion_proof(
    const bool verified_up_to_date,
    const bool byte_identical_null_build = false) {
    return {
        true,
        true,
        true,
        verified_up_to_date,
        byte_identical_null_build};
}

void partial_build_without_inventory_fails_closed(
    const std::filesystem::path& self) {
    TemporaryDirectory temporary;
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        temporary.path(), {2u, 0u, 0u, 2u}, reporter);
    const auto self_text = self.string();
    const std::vector<const char*> arguments{
        "--host-tool-success", "unit.cpp"};
    require(
        katana::cli::run_host_build_tool_launcher(
            katana::cli::HostBuildToolKind::Compile,
            temporary.path(), self_text, arguments) == 0,
        "Compilerlauncher-Probe scheiterte.");
    require(
        !observer.finish_success(completion_proof(false)),
        "Partieller Hostbuild ohne Up-to-date-Inventar wurde akzeptiert.");
    require(reporter.seal_and_flush(), "Fehlerfortschritt ist unvollstaendig.");
}

void exercised_build_reports_exact_verified_cache_hits(
    const std::filesystem::path& self) {
    TemporaryDirectory temporary;
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        temporary.path(),
        {2u, 1u, 1u, 4u},
        reporter);
    const auto self_text = self.string();
    const std::vector<const char*> compile_arguments{
        "--host-tool-success", "unit.cpp"};
    const std::vector<const char*> archive_arguments{
        "--host-tool-success"};
    const std::vector<const char*> link_arguments{
        "--host-tool-success"};
    require(
        katana::cli::run_host_build_tool_launcher(
            katana::cli::HostBuildToolKind::Compile,
            temporary.path(),
            self_text,
            compile_arguments) == 0,
        "Compilerlauncher-Probe scheiterte.");
    require(
        katana::cli::run_host_build_tool_launcher(
            katana::cli::HostBuildToolKind::Archive,
            temporary.path(),
            self_text,
            archive_arguments) == 0,
        "Archivelauncher-Probe scheiterte.");
    require(
        katana::cli::run_host_build_tool_launcher(
            katana::cli::HostBuildToolKind::Link,
            temporary.path(),
            self_text,
            link_arguments) == 0,
        "Linkerlauncher-Probe scheiterte.");
    observer.poll();
    require(
        observer.finish_success(completion_proof(true)),
        "Beobachteter Hostbuild wurde nicht erfolgreich abgeschlossen.");
    const auto counts = observer.snapshot();
    require(
        counts.compile_started == 1u &&
            counts.compile_committed == 1u &&
            counts.archive_started == 1u &&
            counts.archive_committed == 1u &&
            counts.link_started == 1u && counts.link_committed == 1u &&
            counts.observation_complete,
        "Launcherzaehler entsprechen nicht den echten Toolaufrufen.");
    require(reporter.seal_and_flush(), "Hostbuild-Fortschritt ist unvollstaendig.");
    const auto events = log.snapshot();
    const auto& compile = require_terminal(events, "port-host-compile");
    require(
        compile.state == katana::ProgressState::Completed &&
            compile.counters.cache_hits == 1u &&
            compile.counters.ready_work == 1u &&
            compile.counters.committed_work == 1u &&
            compile.counters.queued_work == 0u,
        "Partielle Kompilation trennt echte Commits und Hits nicht exakt.");
    for (const auto label : {
             std::string_view("port-host-archive"),
             std::string_view("port-host-link")}) {
        const auto& terminal = require_terminal(events, label);
        require(
            terminal.state == katana::ProgressState::Completed &&
                !terminal.counters.cache_hits.has_value() &&
                terminal.counters.ready_work == 1u &&
                terminal.counters.committed_work == 1u &&
                terminal.counters.queued_work == 0u,
            "Echter Archiv-/Linkercommit wurde als Hit umgedeutet.");
    }
}

void verified_null_build_is_cached() {
    TemporaryDirectory temporary;
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        temporary.path(),
        {2u, 1u, 1u, 4u},
        reporter);
    require(
        observer.finish_success(completion_proof(true, true)),
        "Byteidentisch verifizierter Nullbuild wurde abgelehnt.");
    require(reporter.seal_and_flush(), "Nullbuild-Fortschritt ist unvollstaendig.");
    const auto events = log.snapshot();
    const auto& compile = require_terminal(events, "port-host-compile");
    const auto& archive = require_terminal(events, "port-host-archive");
    const auto& link = require_terminal(events, "port-host-link");
    require(
        compile.state == katana::ProgressState::Cached &&
            compile.counters.cache_hits == 2u &&
            archive.state == katana::ProgressState::Cached &&
            archive.counters.cache_hits == 1u &&
            link.state == katana::ProgressState::Cached &&
            link.counters.cache_hits == 1u,
        "Verifizierter Nullbuild besitzt keine exakte Cachebilanz.");
}

void incomplete_observation_fails_closed() {
    TemporaryDirectory temporary;
    std::ofstream(temporary.path() / "unknown-event") << "untrusted";
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        temporary.path(),
        {1u, 1u, 1u, 1u},
        reporter);
    require(
        !observer.finish_success(completion_proof(true, true)),
        "Unbekannte Hostbuild-Ereignisse wurden akzeptiert.");
    require(reporter.seal_and_flush(), "Fehlerfortschritt ist unvollstaendig.");
    const auto events = log.snapshot();
    require(
        require_terminal(events, "port-host-compile").state ==
            katana::ProgressState::Failed,
        "Unvollstaendige Beobachtung endet nicht fail-closed.");
}

void replaced_event_root_fails_closed() {
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    const auto replaced_root = temporary.path() / "events-replaced";
    std::filesystem::create_directory(event_root);
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        event_root,
        {1u, 1u, 1u, 1u},
        reporter);
    std::error_code rename_error;
    std::filesystem::rename(event_root, replaced_root, rename_error);
#ifdef _WIN32
    if (rename_error) {
        require(
            observer.finish_success(completion_proof(true, true)),
            "Gepinnte Windows-Ereigniswurzel wurde unbrauchbar.");
        require(
            reporter.seal_and_flush(),
            "Root-Pinning-Fortschritt ist unvollstaendig.");
        return;
    }
#else
    require(!rename_error, "Ereigniswurzel konnte nicht verschoben werden.");
#endif
    std::filesystem::create_directory(event_root);
    require(
        !observer.finish_success(completion_proof(true, true)),
        "Ausgetauschtes Hostbuild-Ereignisverzeichnis wurde akzeptiert.");
    require(
        reporter.seal_and_flush(),
        "Root-Identity-Fehlerfortschritt ist unvollstaendig.");
    require(
        require_terminal(
            log.snapshot(), "port-host-compile").state ==
            katana::ProgressState::Failed,
        "Ausgetauschte Ereigniswurzel endet nicht fail-closed.");
}

void started_to_terminal_rename_is_retried() {
    TemporaryDirectory temporary;
    const auto started = temporary.path() / "event-rename-race.started";
    const auto committed = temporary.path() / "event-rename-race.committed";
    std::ofstream(started, std::ios::binary)
        << "KATANA_HOST_BUILD_EVENT_V1\nkind=compile\nunits=1\n";
    std::atomic_bool renamed = false;
    std::error_code rename_failure;
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        temporary.path(), {1u, 1u, 1u, 1u}, reporter,
        {{[&](const std::filesystem::path& path) {
            if (path == started && !renamed.exchange(true))
                std::filesystem::rename(
                    started, committed, rename_failure);
        }}});
    observer.poll();
    require(
        !rename_failure,
        std::string("Deterministisches Event-Rename scheiterte: ") +
            rename_failure.message());
    const auto live_counts = observer.snapshot();
    require(
        live_counts.observation_complete &&
            live_counts.compile_started == 1u &&
            live_counts.compile_committed == 1u,
        "Live-Retry band das umbenannte Ereignis nicht.");
    require(
        observer.finish_success(completion_proof(true)),
        "Legitimes Started-zu-Terminal-Rename wurde als Verlust gewertet.");
    const auto counts = observer.snapshot();
    require(
        renamed && counts.compile_started == 1u &&
            counts.compile_committed == 1u &&
            counts.observation_complete,
        "Rename-Retry hat das terminale Toolereignis nicht gebunden.");
    require(reporter.seal_and_flush(), "Rename-Race-Fortschritt ist unvollstaendig.");
}

void partial_started_document_is_retried() {
    TemporaryDirectory temporary;
    const auto started = temporary.path() / "event-partial-race.started";
    const auto committed = temporary.path() / "event-partial-race.committed";
    {
        std::ofstream empty_started(started);
    }
    std::atomic_uint32_t openings = 0u;
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        temporary.path(), {1u, 1u, 1u, 1u}, reporter,
        {{[&](const std::filesystem::path& path) {
            if (path != started || ++openings != 2u) return;
            std::ofstream output(
                started, std::ios::binary | std::ios::trunc);
            output << "KATANA_HOST_BUILD_EVENT_V1\nkind=compile\nunits=1\n";
            output.close();
            std::filesystem::rename(started, committed);
        }}});
    observer.poll();
    require(
        observer.finish_success(completion_proof(true)),
        "Legitimes partielles Started-Dokument wurde als Verlust gewertet.");
    require(
        openings >= 2u && observer.snapshot().compile_committed == 1u,
        "Partial-Write-Retry hat das terminale Ereignis nicht gebunden.");
    require(reporter.seal_and_flush(), "Partial-Race-Fortschritt ist unvollstaendig.");
}

void later_poll_observes_new_event(
    const std::filesystem::path& self) {
    TemporaryDirectory temporary;
    EventLog log;
    katana::ProgressReporter reporter(
        [&](const auto& event) { log.append(event); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(20));
    katana::cli::HostBuildProgressObserver observer(
        temporary.path(), {1u, 1u, 1u, 1u}, reporter);
    observer.poll();
    const auto self_text = self.string();
    const std::vector<const char*> arguments{
        "--host-tool-success", "late.cpp"};
    require(
        katana::cli::run_host_build_tool_launcher(
            katana::cli::HostBuildToolKind::Compile,
            temporary.path(), self_text, arguments) == 0,
        "Spaeter Compilerlauncher-Aufruf scheiterte.");
    observer.poll();
    require(
        observer.snapshot().compile_committed == 1u,
        "Zweiter Poll sah das nachtraegliche Ereignis nicht.");
    require(
        observer.finish_success(completion_proof(true)),
        "Spaeteres Ereignis konnte nicht terminal gebunden werden.");
    require(reporter.seal_and_flush(), "Mehrfachscan-Fortschritt ist unvollstaendig.");
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc >= 2 &&
        std::string_view(argv[1]) == "--host-tool-success")
        return EXIT_SUCCESS;
    try {
        const auto self =
            std::filesystem::absolute(argv[0]).lexically_normal();
        partial_build_without_inventory_fails_closed(self);
        exercised_build_reports_exact_verified_cache_hits(self);
        verified_null_build_is_cached();
        incomplete_observation_fails_closed();
        replaced_event_root_fails_closed();
        started_to_terminal_rename_is_retried();
        partial_started_document_is_retried();
        later_poll_observes_new_event(self);
        std::cout << "Hostbuild-Fortschrittsvertrag erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
