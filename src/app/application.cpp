#include "katana/app/application.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/executable_inventory.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/port_export.hpp"
#include "katana/codegen/project.hpp"
#include "katana/io/input_output_error.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/platform/firmware_profile.hpp"
#include "katana/runtime/gdi.hpp"
#include "katana/runtime/packed_disc.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace katana::app {
namespace {

bool outputs_overlap(const std::filesystem::path& left, const std::filesystem::path& right);

class CrossProcessJobLock final {
  public:
    explicit CrossProcessJobLock(std::filesystem::path output) : output_(std::move(output)) {
#ifdef _WIN32
        registry_mutex_ =
            CreateMutexW(nullptr, FALSE, L"Local\\KatanaRecomp-OutputLockRegistry-v1");
        if (registry_mutex_ == nullptr)
            throw std::runtime_error("Ausgabe-Lockregistry konnte nicht erzeugt werden.");
        const auto wait = WaitForSingleObject(registry_mutex_, INFINITE);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(registry_mutex_);
            registry_mutex_ = nullptr;
            throw std::runtime_error("Ausgabe-Lockregistry ist nicht erreichbar.");
        }
        try {
            const auto registry =
                std::filesystem::temp_directory_path() / "KatanaRecomp-output-locks-v1";
            std::filesystem::create_directories(registry);
            for (const auto& entry : std::filesystem::directory_iterator(registry)) {
                if (!entry.is_regular_file()) continue;
                std::ifstream input(entry.path(), std::ios::binary);
                std::string pid_text;
                std::string path_text;
                std::getline(input, pid_text);
                std::getline(input, path_text);
                bool live = false;
                try {
                    const auto pid = static_cast<DWORD>(std::stoul(pid_text));
                    if (const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
                        live = WaitForSingleObject(process, 0u) == WAIT_TIMEOUT;
                        CloseHandle(process);
                    } else {
                        live = GetLastError() == ERROR_ACCESS_DENIED;
                    }
                } catch (const std::exception&) {
                    live = false;
                }
                if (!live) {
                    std::error_code remove_error;
                    std::filesystem::remove(entry.path(), remove_error);
                    continue;
                }
                if (outputs_overlap(output_, std::filesystem::path(path_text))) {
                    throw std::runtime_error(
                        "Ein anderer Prozess verwendet ein ueberlappendes Ausgabeziel.");
                }
            }
            lock_file_ =
                registry / (io::sha256_bytes(output_.generic_string()).substr(0u, 24u) + ".lock");
            std::ofstream output_file(lock_file_, std::ios::binary | std::ios::trunc);
            if (!output_file)
                throw std::runtime_error("Ausgabe-Lockdatei konnte nicht erzeugt werden.");
            output_file << GetCurrentProcessId() << '\n' << output_.generic_string() << '\n';
            if (!output_file)
                throw std::runtime_error("Ausgabe-Lockdatei konnte nicht geschrieben werden.");
        } catch (...) {
            ReleaseMutex(registry_mutex_);
            CloseHandle(registry_mutex_);
            registry_mutex_ = nullptr;
            throw;
        }
        ReleaseMutex(registry_mutex_);
#else
        const auto registry =
            std::filesystem::temp_directory_path() / "KatanaRecomp-output-locks-v1";
        std::filesystem::create_directories(registry);
        const auto registry_path = registry.parent_path() / "KatanaRecomp-output-locks-v1.registry";
        const auto registry_fd = ::open(registry_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (registry_fd < 0 || ::flock(registry_fd, LOCK_EX) != 0) {
            if (registry_fd >= 0) ::close(registry_fd);
            throw std::runtime_error("Ausgabe-Lockregistry ist nicht erreichbar.");
        }
        try {
            for (const auto& entry : std::filesystem::directory_iterator(registry)) {
                if (!entry.is_regular_file()) continue;
                std::ifstream input(entry.path(), std::ios::binary);
                std::string pid_text;
                std::string path_text;
                std::getline(input, pid_text);
                std::getline(input, path_text);
                bool live = false;
                try {
                    const auto pid = static_cast<pid_t>(std::stol(pid_text));
                    live = pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
                } catch (const std::exception&) {
                    live = false;
                }
                if (!live) {
                    std::error_code remove_error;
                    std::filesystem::remove(entry.path(), remove_error);
                    continue;
                }
                if (outputs_overlap(output_, std::filesystem::path(path_text))) {
                    throw std::runtime_error(
                        "Ein anderer Prozess verwendet ein ueberlappendes Ausgabeziel.");
                }
            }
            lock_file_ =
                registry / (io::sha256_bytes(output_.generic_string()).substr(0u, 24u) + ".lock");
            std::ofstream output_file(lock_file_, std::ios::binary | std::ios::trunc);
            if (!output_file)
                throw std::runtime_error("Ausgabe-Lockdatei konnte nicht erzeugt werden.");
            output_file << ::getpid() << '\n' << output_.generic_string() << '\n';
            if (!output_file)
                throw std::runtime_error("Ausgabe-Lockdatei konnte nicht geschrieben werden.");
        } catch (...) {
            static_cast<void>(::flock(registry_fd, LOCK_UN));
            ::close(registry_fd);
            throw;
        }
        static_cast<void>(::flock(registry_fd, LOCK_UN));
        ::close(registry_fd);
#endif
    }
    ~CrossProcessJobLock() {
#ifdef _WIN32
        if (registry_mutex_ != nullptr) {
            const auto wait = WaitForSingleObject(registry_mutex_, INFINITE);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                std::error_code remove_error;
                std::filesystem::remove(lock_file_, remove_error);
                ReleaseMutex(registry_mutex_);
            }
            CloseHandle(registry_mutex_);
        }
#else
        const auto registry_path =
            lock_file_.parent_path().parent_path() / "KatanaRecomp-output-locks-v1.registry";
        const auto registry_fd = ::open(registry_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (registry_fd >= 0) {
            if (::flock(registry_fd, LOCK_EX) == 0) {
                std::error_code remove_error;
                std::filesystem::remove(lock_file_, remove_error);
                static_cast<void>(::flock(registry_fd, LOCK_UN));
            }
            ::close(registry_fd);
        }
#endif
    }
    CrossProcessJobLock(const CrossProcessJobLock&) = delete;
    CrossProcessJobLock& operator=(const CrossProcessJobLock&) = delete;

  private:
    std::filesystem::path output_;
#ifdef _WIN32
    HANDLE registry_mutex_ = nullptr;
    std::filesystem::path lock_file_;
#else
    std::filesystem::path lock_file_;
#endif
};

std::filesystem::path normalized_output_path(std::filesystem::path path) {
    path = std::filesystem::absolute(path).lexically_normal();
    std::vector<std::filesystem::path> missing;
    std::error_code error;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        if (error) break;
        missing.push_back(path.filename());
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    if (!path.empty()) {
        auto resolved = std::filesystem::canonical(path, error);
        if (!error) {
            for (auto iterator = missing.rbegin(); iterator != missing.rend(); ++iterator)
                resolved /= *iterator;
            path = resolved.lexically_normal();
        }
    }
#ifdef _WIN32
    auto text = path.wstring();
    std::transform(text.begin(), text.end(), text.begin(), ::towlower);
    path = std::filesystem::path(text);
#endif
    return path;
}

bool outputs_overlap(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto within = [](const auto& path, const auto& root) {
        const auto relative = path.lexically_relative(root);
        return relative.empty() || (!relative.is_absolute() && *relative.begin() != "..");
    };
    return within(left, right) || within(right, left);
}

void require_stable_id(const std::string_view value, const char* field) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](const unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.';
        })) {
        throw std::invalid_argument(std::string(field) + " ist kein stabiler Bezeichner.");
    }
}

void write_atomic(const std::filesystem::path& path, const std::string_view content) {
    if (path.empty()) throw std::invalid_argument("Ausgabepfad fehlt.");
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".katana-tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Temporare Ausgabedatei ist nicht schreibbar.");
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) throw std::runtime_error("Temporare Ausgabedatei ist unvollstaendig.");
    }
#ifdef _WIN32
    const auto replaced =
        std::filesystem::exists(path)
            ? ReplaceFileW(path.c_str(),
                           temporary.c_str(),
                           nullptr,
                           REPLACEFILE_WRITE_THROUGH,
                           nullptr,
                           nullptr)
            : MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!replaced) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Ausgabedatei konnte nicht atomar ersetzt werden.");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Ausgabedatei konnte nicht atomar ersetzt werden.");
    }
#endif
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Datei ist nicht lesbar.");
    std::ostringstream output;
    output << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Datei konnte nicht vollstaendig gelesen werden.");
    return output.str();
}

std::string portable_name(const std::filesystem::path& path) {
    return path.filename().generic_string();
}

Diagnostic make_error(std::string code, const std::string_view message, std::string recovery) {
    return {DiagnosticSeverity::Error,
            std::move(code),
            redact_sensitive_text(message),
            std::move(recovery),
            std::nullopt};
}

class JobEventStream final {
  public:
    JobEventStream(const JobRequest& request, const JobObserver& observer)
        : request_(request), observer_(observer), started_(std::chrono::steady_clock::now()) {}

    void emit(const JobState state,
              const std::uint32_t progress,
              std::string stage,
              std::optional<Diagnostic> diagnostic = std::nullopt,
              JobStepStatus step_status = JobStepStatus::Running,
              std::optional<std::uint64_t> current = std::nullopt,
              std::optional<std::uint64_t> total = std::nullopt,
              std::optional<std::string> log_chunk = std::nullopt) {
        if (!observer_) return;
        if (total && (!current || *current > *total))
            throw std::logic_error("Jobfortschrittszaehler ist ungueltig.");
        last_progress_ = std::max(last_progress_, progress);
        if (state == JobState::Running && step_status == JobStepStatus::Running)
            active_stage_ = stage;
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto elapsed = std::chrono::steady_clock::now() - started_;
        observer_({request_.id,
                   sequence_++,
                   state,
                   last_progress_,
                   std::move(stage),
                   step_status,
                   current,
                   total,
                   static_cast<std::uint64_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(now).count()),
                   static_cast<std::uint64_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
                   std::move(log_chunk),
                   std::move(diagnostic)});
    }

    [[nodiscard]] const std::string& active_stage() const noexcept {
        return active_stage_;
    }

  private:
    const JobRequest& request_;
    const JobObserver& observer_;
    std::chrono::steady_clock::time_point started_;
    std::uint64_t sequence_ = 0u;
    std::uint32_t last_progress_ = 0u;
    std::string active_stage_ = "queued";
};

void require_not_cancelled(const std::shared_ptr<Cancellation>& cancellation) {
    if (cancellation && cancellation->requested()) throw JobState::Cancelled;
}

#ifdef _WIN32
void terminate_process_tree(const DWORD root_process) noexcept {
    std::vector<std::pair<DWORD, DWORD>> processes;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                processes.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    std::unordered_set<DWORD> descendants{root_process};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [process, parent] : processes) {
            if (descendants.contains(parent) && descendants.insert(process).second) changed = true;
        }
    }
    for (auto iterator = processes.rbegin(); iterator != processes.rend(); ++iterator) {
        if (!descendants.contains(iterator->first) || iterator->first == root_process) continue;
        if (const auto child =
                OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, iterator->first)) {
            TerminateProcess(child, 1u);
            WaitForSingleObject(child, 2'000u);
            CloseHandle(child);
        }
    }
    if (const auto root = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, root_process)) {
        TerminateProcess(root, 1u);
        WaitForSingleObject(root, 2'000u);
        CloseHandle(root);
    }
}
#endif

std::filesystem::path discover_runtime_root() {
#ifdef _WIN32
    char* configured = nullptr;
    std::size_t configured_size = 0u;
    if (_dupenv_s(&configured, &configured_size, "KATANA_RUNTIME_ROOT") == 0 &&
        configured != nullptr) {
        const auto result = std::filesystem::path(configured);
        std::free(configured);
        if (!result.empty()) return result;
    }
#else
    if (const auto* configured = std::getenv("KATANA_RUNTIME_ROOT");
        configured != nullptr && *configured != '\0')
        return std::filesystem::path(configured);
#endif
    std::filesystem::path executable;
#ifdef _WIN32
    std::wstring buffer(32'768u, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0u && length < buffer.size()) {
        buffer.resize(length);
        executable = std::filesystem::path(buffer);
    }
#else
    std::error_code link_error;
    executable = std::filesystem::read_symlink("/proc/self/exe", link_error);
#endif
    if (!executable.empty()) {
        const auto directory = executable.parent_path();
        for (const auto& candidate : {directory / "runtime-sdk", directory.parent_path()}) {
            if (std::filesystem::exists(candidate / "CMakeLists.txt") &&
                std::filesystem::exists(candidate / "include" / "katana" / "runtime")) {
                return candidate;
            }
        }
    }
    throw std::runtime_error(
        "Katana Runtime-SDK fehlt neben der Anwendung; KATANA_RUNTIME_ROOT kann es explizit "
        "angeben.");
}

struct ProjectSnapshot {
    SourceInspection inspection;
    std::vector<io::InputProvenance> inputs;
};

ProjectSnapshot capture_project_snapshot(const io::ProjectManifest& manifest,
                                         const std::shared_ptr<Cancellation>& cancellation = {}) {
    ProjectSnapshot snapshot;
    auto& result = snapshot.inspection;
    result.format = io::project_input_format_name(manifest.format);
    result.display_name = portable_name(manifest.input_path);
    if (manifest.format != io::ProjectInputFormat::DreamcastGdi) {
        const auto provenance = io::capture_input_provenance(
            "project-input", manifest.input_path, [&] { require_not_cancelled(cancellation); });
        result.size = provenance.size;
        result.sha256 = provenance.sha256;
        snapshot.inputs.push_back(provenance);
    } else {
        const auto descriptor = runtime::parse_gdi_descriptor(manifest.input_path);
        const auto descriptor_provenance = io::capture_input_provenance(
            "gdi-descriptor", manifest.input_path, [&] { require_not_cancelled(cancellation); });
        result.size = descriptor_provenance.size;
        result.sha256 = descriptor_provenance.sha256;
        snapshot.inputs.push_back(descriptor_provenance);
        for (const auto& track : descriptor.tracks) {
            const auto provenance =
                io::capture_input_provenance("gdi-track-" + std::to_string(track.number),
                                             track.resolved_path,
                                             [&] { require_not_cancelled(cancellation); });
            snapshot.inputs.push_back(provenance);
            result.tracks.push_back({track.number,
                                     track.lba,
                                     track.type == runtime::GdiTrackType::Data ? "data" : "audio",
                                     track.sector_size,
                                     provenance.size,
                                     track.file_offset,
                                     track.sector_count,
                                     track.descriptor_line,
                                     portable_name(track.resolved_path),
                                     provenance.sha256});
        }
    }
    const auto capture_optional =
        [&snapshot, &cancellation](const char* role,
                                   const std::optional<std::filesystem::path>& path) {
            if (path)
                snapshot.inputs.push_back(io::capture_input_provenance(
                    role, *path, [&] { require_not_cancelled(cancellation); }));
        };
    capture_optional("symbol-map", manifest.map_path);
    capture_optional("analysis-overrides", manifest.analysis_overrides_path);
    capture_optional("firmware-bios", manifest.bios_path);
    capture_optional("firmware-flash", manifest.flash_path);
    return snapshot;
}

bool same_snapshot(const ProjectSnapshot& left, const ProjectSnapshot& right) {
    if (left.inputs.size() != right.inputs.size()) return false;
    const auto portable = [](const ProjectSnapshot& snapshot) {
        std::vector<std::string> values;
        values.reserve(snapshot.inputs.size());
        for (const auto& input : snapshot.inputs) {
            values.push_back(input.role + ':' + std::to_string(input.size) + ':' + input.sha256);
        }
        std::sort(values.begin(), values.end());
        return values;
    };
    return portable(left) == portable(right);
}

std::string project_identity(const io::ProjectManifest& manifest, const ProjectSnapshot& snapshot) {
    auto portable_profile = manifest;
    const auto portable_root = std::filesystem::current_path() / "katana-portable-identity";
    const auto portable_path = [&](const std::filesystem::path& path) {
        return portable_root / path.filename();
    };
    portable_profile.input_path = portable_path(manifest.input_path);
    if (manifest.map_path) portable_profile.map_path = portable_path(*manifest.map_path);
    if (manifest.analysis_overrides_path)
        portable_profile.analysis_overrides_path = portable_path(*manifest.analysis_overrides_path);
    if (manifest.bios_path) portable_profile.bios_path = portable_path(*manifest.bios_path);
    if (manifest.flash_path) portable_profile.flash_path = portable_path(*manifest.flash_path);
    const auto portable_manifest =
        io::serialize_project_manifest(portable_profile, portable_root / "project.katana");
    std::ostringstream identity;
    identity << portable_manifest;
    std::vector<std::string> inputs;
    inputs.reserve(snapshot.inputs.size());
    for (const auto& input : snapshot.inputs) {
        inputs.push_back(input.role + ':' + std::to_string(input.size) + ':' + input.sha256);
    }
    std::sort(inputs.begin(), inputs.end());
    for (const auto& input : inputs)
        identity << "\ninput=" << input;
    return io::sha256_bytes(identity.str());
}

std::string hex_address(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::string shell_quote(const std::filesystem::path& path) {
    const auto text = path.string();
#ifdef _WIN32
    if (text.find('"') != std::string::npos)
        throw std::invalid_argument("Hostbuildpfad enthaelt ein Anfuehrungszeichen.");
    return '"' + text + '"';
#else
    std::string quoted = "'";
    for (const auto character : text)
        character == '\'' ? quoted += "'\\''" : quoted += character;
    return quoted + "'";
#endif
}

void run_host_command(const std::string& command,
                      const char* stage,
                      const std::filesystem::path& log_path,
                      const std::shared_ptr<Cancellation>& cancellation,
                      JobEventStream& events,
                      const std::uint32_t overall_progress,
                      const std::string_view event_stage,
                      std::uint64_t& log_offset) {
    int status = 0;
    std::string pending_log;
    const auto emit_log = [&](const bool flush) {
        if (!std::filesystem::exists(log_path)) return;
        std::ifstream input(log_path, std::ios::binary);
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0 || static_cast<std::uint64_t>(end) <= log_offset) {
            if (flush && !pending_log.empty()) {
                events.emit(JobState::Running,
                            overall_progress,
                            std::string(event_stage),
                            {},
                            JobStepStatus::Running,
                            {},
                            {},
                            redact_sensitive_text(pending_log));
                pending_log.clear();
            }
            return;
        }
        input.seekg(static_cast<std::streamoff>(log_offset));
        std::string appended((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
        log_offset = static_cast<std::uint64_t>(end);
        pending_log += appended;
        auto emit_size = pending_log.size();
        if (!flush) {
            const auto newline = pending_log.find_last_of("\r\n");
            if (newline == std::string::npos) return;
            emit_size = newline + 1u;
        }
        auto chunk = pending_log.substr(0u, emit_size);
        pending_log.erase(0u, emit_size);
        if (!chunk.empty())
            events.emit(JobState::Running,
                        overall_progress,
                        std::string(event_stage),
                        {},
                        JobStepStatus::Running,
                        {},
                        {},
                        redact_sensitive_text(chunk));
    };
#ifdef _WIN32
    auto command_line = command;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const auto log_handle = CreateFileW(log_path.c_str(),
                                        FILE_APPEND_DATA,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &security,
                                        OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    const auto input_handle = CreateFileW(L"NUL",
                                          GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &security,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
    if (log_handle == INVALID_HANDLE_VALUE || input_handle == INVALID_HANDLE_VALUE) {
        if (log_handle != INVALID_HANDLE_VALUE) CloseHandle(log_handle);
        if (input_handle != INVALID_HANDLE_VALUE) CloseHandle(input_handle);
        throw std::runtime_error("Hostbuild-Ein-/Ausgabe konnte nicht geoeffnet werden.");
    }
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_handle;
    startup.hStdOutput = log_handle;
    startup.hStdError = log_handle;
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr,
                        command_line.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startup,
                        &process)) {
        CloseHandle(input_handle);
        CloseHandle(log_handle);
        throw std::runtime_error("Hostbuild-Prozess konnte nicht gestartet werden.");
    }
    for (;;) {
        const auto wait = WaitForSingleObject(process.hProcess, 50u);
        emit_log(false);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            terminate_process_tree(process.dwProcessId);
            status = 1;
            break;
        }
        if (cancellation && cancellation->requested()) {
            terminate_process_tree(process.dwProcessId);
            WaitForSingleObject(process.hProcess, 5'000u);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(input_handle);
            CloseHandle(log_handle);
            throw JobState::Cancelled;
        }
    }
    DWORD exit_code = 1u;
    GetExitCodeProcess(process.hProcess, &exit_code);
    status = static_cast<int>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(input_handle);
    CloseHandle(log_handle);
#else
    require_not_cancelled(cancellation);
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("Hostbuild-Prozess konnte nicht gestartet werden.");
    if (child == 0) {
        static_cast<void>(::setpgid(0, 0));
        const auto descriptor =
            ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, static_cast<mode_t>(0600));
        if (descriptor < 0) ::_exit(127);
        static_cast<void>(::dup2(descriptor, STDOUT_FILENO));
        static_cast<void>(::dup2(descriptor, STDERR_FILENO));
        ::close(descriptor);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    static_cast<void>(::setpgid(child, child));
    int wait_status = 0;
    bool wait_succeeded = false;
    for (;;) {
        const auto waited = ::waitpid(child, &wait_status, WNOHANG);
        emit_log(false);
        if (waited == child) {
            wait_succeeded = true;
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) continue;
            status = 1;
            break;
        }
        if (cancellation && cancellation->requested()) {
            static_cast<void>(::kill(-child, SIGTERM));
            bool terminated = false;
            for (std::size_t attempt = 0u; attempt < 100u; ++attempt) {
                const auto stopped = ::waitpid(child, &wait_status, WNOHANG);
                if (stopped == child) {
                    terminated = true;
                    break;
                }
                if (stopped < 0 && errno != EINTR) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!terminated) {
                static_cast<void>(::kill(-child, SIGKILL));
                for (;;) {
                    const auto stopped = ::waitpid(child, &wait_status, 0);
                    if (stopped == child || (stopped < 0 && errno != EINTR)) break;
                }
            }
            throw JobState::Cancelled;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (wait_succeeded && WIFEXITED(wait_status))
        status = WEXITSTATUS(wait_status);
    else if (wait_succeeded)
        status = 1;
#endif
    emit_log(true);
    auto log = std::filesystem::exists(log_path) ? read_text(log_path) : std::string{};
    log = redact_sensitive_text(log);
    write_atomic(log_path, log);
    log_offset = static_cast<std::uint64_t>(std::filesystem::file_size(log_path));
    if (status != 0) {
        constexpr std::size_t diagnostic_limit = 4'000u;
        const auto tail =
            log.size() <= diagnostic_limit ? log : log.substr(log.size() - diagnostic_limit);
        throw std::runtime_error(std::string("Hostbuild ist fehlgeschlagen: ") + stage + ".\n" +
                                 tail);
    }
}

void configure_and_build(const std::filesystem::path& source,
                         const std::filesystem::path& build,
                         const std::filesystem::path& runtime_root,
                         const std::string_view target,
                         const std::filesystem::path& log_path,
                         const std::shared_ptr<Cancellation>& cancellation,
                         JobEventStream& events) {
    std::uint64_t log_offset = 0u;
    auto configure = std::string("cmake -S ") + shell_quote(source) + " -B " + shell_quote(build);
#ifdef _WIN32
    char* requested_generator = nullptr;
    std::size_t requested_generator_size = 0u;
    static_cast<void>(
        _dupenv_s(&requested_generator, &requested_generator_size, "KATANA_HOST_BUILD_GENERATOR"));
    const bool use_ninja =
        requested_generator != nullptr && std::string_view(requested_generator) == "Ninja";
    std::free(requested_generator);
    if (use_ninja) {
        configure += " -G Ninja -DCMAKE_BUILD_TYPE=Debug";
        char* requested_make_program = nullptr;
        std::size_t requested_make_program_size = 0u;
        static_cast<void>(_dupenv_s(&requested_make_program,
                                    &requested_make_program_size,
                                    "KATANA_HOST_BUILD_MAKE_PROGRAM"));
        if (requested_make_program != nullptr && *requested_make_program != '\0')
            configure += " -DCMAKE_MAKE_PROGRAM=" +
                         shell_quote(std::filesystem::path(requested_make_program));
        std::free(requested_make_program);
    } else {
        configure += " -G \"Visual Studio 17 2022\" -A x64";
    }
#else
    configure += " -G Ninja -DCMAKE_BUILD_TYPE=Debug";
    if (const auto* requested_make_program = std::getenv("KATANA_HOST_BUILD_MAKE_PROGRAM");
        requested_make_program != nullptr && *requested_make_program != '\0')
        configure +=
            " -DCMAKE_MAKE_PROGRAM=" + shell_quote(std::filesystem::path(requested_make_program));
#endif
    configure += " -DKATANA_RUNTIME_ROOT=" + shell_quote(runtime_root);
    try {
        run_host_command(configure,
                         "configure",
                         log_path,
                         cancellation,
                         events,
                         78u,
                         "host-configuration",
                         log_offset);
    } catch (const std::runtime_error& error) {
        std::string detail;
        for (const auto& name : {"CMakeConfigureLog.yaml", "CMakeError.log", "CMakeOutput.log"}) {
            const auto configure_log = build / "CMakeFiles" / name;
            if (std::filesystem::exists(configure_log)) detail += read_text(configure_log);
        }
        if (detail.empty()) throw;
        constexpr std::size_t detail_limit = 8'000u;
        if (detail.size() > detail_limit) detail.erase(0u, detail.size() - detail_limit);
        throw std::runtime_error(std::string(error.what()) + "\nCMake-Konfigurationsdetail:\n" +
                                 detail);
    }
    events.emit(JobState::Running, 80u, "host-configuration", {}, JobStepStatus::Completed, 1u, 1u);
    events.emit(JobState::Running, 80u, "host-compilation", {}, JobStepStatus::Running);
    auto compile =
        std::string("cmake --build ") + shell_quote(build) + " --target " + std::string(target);
#ifdef _WIN32
    if (!use_ninja) compile += " --config Debug";
#endif
    run_host_command(
        compile, "compile", log_path, cancellation, events, 90u, "host-compilation", log_offset);
}

std::string result_index_json(const io::LoadedProject& project,
                              const analysis::ControlFlowAnalysisResult& analysis,
                              const SourceInspection& source,
                              const std::string_view identity) {
    auto functions = analysis.recursive.functions;
    std::sort(functions.begin(), functions.end(), [](const auto& left, const auto& right) {
        return left.address < right.address;
    });
    std::ostringstream output;
    output << "{\"schema\":\"katana-result-index\",\"version\":1,\"project_identity\":"
           << io::quote_json(identity)
           << ",\"source\":{\"name\":" << io::quote_json(source.display_name)
           << ",\"format\":" << io::quote_json(source.format)
           << ",\"sha256\":" << io::quote_json(source.sha256) << "},\"functions\":[";
    for (std::size_t index = 0u; index < functions.size(); ++index) {
        if (index != 0u) output << ',';
        output << "{\"address\":" << io::quote_json(hex_address(functions[index].address))
               << ",\"confidence\":"
               << io::quote_json(analysis::analysis_confidence_name(functions[index].confidence))
               << ",\"evidence\":"
               << io::quote_json(analysis::control_flow_evidence_name(functions[index].evidence))
               << '}';
    }
    output << "],\"segments\":[";
    const auto segments = project.image.segments();
    for (std::size_t index = 0u; index < segments.size(); ++index) {
        if (index != 0u) output << ',';
        output << "{\"name\":" << io::quote_json(segments[index].name)
               << ",\"address\":" << io::quote_json(hex_address(segments[index].virtual_address))
               << ",\"size\":" << segments[index].memory_size
               << ",\"kind\":" << io::quote_json(io::segment_kind_name(segments[index].kind))
               << '}';
    }
    output << "],\"provenance\":{\"firmware_mode\":"
           << io::quote_json(
                  io::project_firmware_mode_name(project.execution_profile.firmware_mode))
           << ",\"alias_groups\":" << project.execution_profile.alias_groups.size()
           << ",\"dynamic_vectors\":" << project.execution_profile.dynamic_bios_vectors.size()
           << ",\"writable_executable_ranges\":"
           << project.execution_profile.writable_executable_ranges.size() << "}}\n";
    return output.str();
}

std::string artifact_hash(const std::filesystem::path& path) {
    return io::capture_input_provenance("job-artifact", path).sha256;
}

AnalysisCoverage analysis_coverage(const io::LoadedProject& project,
                                   const analysis::ControlFlowAnalysisResult& analysis,
                                   const analysis::ExecutableByteInventory& inventory) {
    AnalysisCoverage coverage;
    coverage.committed_executable_bytes = inventory.committed_executable_bytes;
    coverage.committed_executable_permission_bytes = inventory.committed_executable_bytes;
    coverage.executable_byte_classes = inventory.byte_counts;
    coverage.precompile_classes = inventory.precompile_counts;
    coverage.mixed_range_roles = inventory.role_counts;
    coverage.range_proof_classes = inventory.proof_counts;
    coverage.instructions = analysis.recursive.instructions.size();
    coverage.proven_instructions = analysis.recursive.proven_instruction_addresses.size();
    coverage.guarded_candidate_instructions =
        analysis.recursive.guarded_candidate_instruction_addresses.size();
    coverage.analyzed_instruction_bytes = coverage.instructions * 2u;
    coverage.static_precompiled_bytes = coverage.analyzed_instruction_bytes;
    coverage.initially_required_bytes = coverage.precompile_classes[static_cast<std::size_t>(
        analysis::PrecompileClass::InitiallyReachable)];
    coverage.currently_dispatchable_bytes = coverage.static_precompiled_bytes;
    coverage.incomplete_initial_required_code_bytes =
        coverage.initially_required_bytes > coverage.static_precompiled_bytes
            ? coverage.initially_required_bytes - coverage.static_precompiled_bytes
            : 0u;
    coverage.runtime_deferred_executable_bytes =
        coverage.precompile_classes[static_cast<std::size_t>(
            analysis::PrecompileClass::LoadableModule)] +
        coverage.precompile_classes[static_cast<std::size_t>(
            analysis::PrecompileClass::RuntimeMaterializable)];
    coverage.runtime_materializable_bytes = coverage.runtime_deferred_executable_bytes;
    coverage.never_executed_data_bytes = coverage.precompile_classes[static_cast<std::size_t>(
        analysis::PrecompileClass::NeverExecutedData)];
    coverage.unknown_executable_bytes = coverage.executable_byte_classes[static_cast<std::size_t>(
                                            analysis::ExecutableByteClass::UnknownExecutable)] +
                                        coverage.executable_byte_classes[static_cast<std::size_t>(
                                            analysis::ExecutableByteClass::CompressedOrEncoded)];
    for (const auto& range : inventory.ranges) {
        if (range.byte_class == analysis::ExecutableByteClass::Padding &&
            range.proof != analysis::RangeProofClass::Proven)
            coverage.unproven_padding_bytes += range.size;
        if (range.precompile_class == analysis::PrecompileClass::LoadableModule ||
            range.precompile_class == analysis::PrecompileClass::RuntimeMaterializable) {
            const auto& segment = project.image.segments()[range.segment_index];
            if (segment.source_kind == io::ImageSourceKind::Unknown ||
                segment.local_source_name.empty())
                coverage.uncovered_runtime_materializable_bytes += range.size;
        }
    }
    coverage.unknown_storage_bytes =
        coverage.precompile_classes[static_cast<std::size_t>(analysis::PrecompileClass::Unknown)];
    coverage.unanalyzed_executable_bytes = coverage.unknown_storage_bytes +
                                           coverage.incomplete_initial_required_code_bytes +
                                           coverage.uncovered_runtime_materializable_bytes;
    coverage.functions = analysis.recursive.functions.size();
    coverage.unknown_instructions = analysis.recursive.diagnostics.size();
    for (const auto& resolution : analysis.indirect_control_flow) {
        switch (analysis::control_flow_report_status(resolution)) {
        case analysis::ControlFlowReportStatus::Resolved:
            ++coverage.resolved_control_flow;
            break;
        case analysis::ControlFlowReportStatus::GuardedComplete:
            ++coverage.guarded_complete_control_flow;
            ++coverage.guarded_control_flow;
            break;
        case analysis::ControlFlowReportStatus::GuardedPartial:
            ++coverage.guarded_partial_control_flow;
            ++coverage.guarded_control_flow;
            break;
        case analysis::ControlFlowReportStatus::RuntimeOnly:
            ++coverage.runtime_only_control_flow;
            break;
        case analysis::ControlFlowReportStatus::Unresolved:
            ++coverage.unresolved_control_flow;
            break;
        }
    }
    coverage.reachable_abort_edges = coverage.unknown_instructions +
                                     coverage.guarded_partial_control_flow +
                                     coverage.unresolved_control_flow;
    std::set<std::pair<std::uint32_t, std::uint32_t>> invalid_edges;
    const auto add_invalid_edge = [&](const std::uint32_t source, const std::uint32_t target) {
        if (!analysis::validate_committed_code_address(project.image, target).valid())
            invalid_edges.emplace(source, target);
    };
    for (const auto& line : analysis.recursive.instructions) {
        if (!line.instruction.is_known() || line.is_delay_slot) continue;
        const auto distance = line.instruction.has_delay_slot ? 4u : 2u;
        if (line.instruction.has_delay_slot) add_invalid_edge(line.address, line.address + 2u);
        switch (line.instruction.control_flow) {
        case sh4::ControlFlowKind::None:
            add_invalid_edge(line.address, line.address + 2u);
            break;
        case sh4::ControlFlowKind::ConditionalBranch:
        case sh4::ControlFlowKind::Call:
            if (line.target_address) add_invalid_edge(line.address, *line.target_address);
            add_invalid_edge(line.address, line.address + distance);
            break;
        case sh4::ControlFlowKind::IndirectCall:
            add_invalid_edge(line.address, line.address + distance);
            break;
        case sh4::ControlFlowKind::UnconditionalBranch:
            if (line.target_address) add_invalid_edge(line.address, *line.target_address);
            break;
        case sh4::ControlFlowKind::IndirectBranch:
        case sh4::ControlFlowKind::Return:
        case sh4::ControlFlowKind::Trap:
        case sh4::ControlFlowKind::ExceptionReturn:
        case sh4::ControlFlowKind::Halt:
            break;
        }
    }
    coverage.reachable_abort_edges += invalid_edges.size();
    std::unordered_set<std::uint32_t> compiled_targets;
    for (const auto& line : analysis.recursive.instructions)
        compiled_targets.insert(line.address);
    std::set<std::uint32_t> required_targets(project.image.entry_points().begin(),
                                             project.image.entry_points().end());
    for (const auto& line : analysis.recursive.instructions)
        if (line.target_address.has_value()) required_targets.insert(*line.target_address);
    for (const auto& edge : analysis.resolved_edges)
        required_targets.insert(edge.target_address);
    const auto runtime_provenance_covers = [&](const std::uint32_t target) {
        return std::any_of(
            inventory.ranges.begin(), inventory.ranges.end(), [&](const auto& range) {
                const auto end = static_cast<std::uint64_t>(range.address) + range.size;
                if (target < range.address || target >= end) return false;
                if (range.precompile_class != analysis::PrecompileClass::LoadableModule &&
                    range.precompile_class != analysis::PrecompileClass::RuntimeMaterializable)
                    return false;
                const auto& segment = project.image.segments()[range.segment_index];
                return segment.source_kind != io::ImageSourceKind::Unknown &&
                       !segment.local_source_name.empty();
            });
    };
    for (const auto target : required_targets)
        if (!compiled_targets.contains(target) && !runtime_provenance_covers(target))
            ++coverage.uncovered_control_targets;
    // All generated direct, indirect, return, exception and interrupt transfers use the
    // validating runtime dispatcher. Contract regressions guard this value against drift.
    coverage.dispatch_paths_without_validation = 0u;
    coverage.control_flow_complete =
        coverage.unknown_instructions == 0u && coverage.guarded_partial_control_flow == 0u &&
        coverage.unresolved_control_flow == 0u && coverage.reachable_abort_edges == 0u &&
        coverage.incomplete_initial_required_code_bytes == 0u &&
        coverage.uncovered_runtime_materializable_bytes == 0u &&
        coverage.uncovered_control_targets == 0u &&
        coverage.dispatch_paths_without_validation == 0u;
    return coverage;
}

std::string build_plan_json(const std::string_view status,
                            const std::string_view tool_version,
                            const AnalysisCoverage& coverage,
                            const bool host_compilation) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-build-plan\",\"version\":7,\"status\":"
           << io::quote_json(status) << ",\"tool_version\":" << io::quote_json(tool_version)
           << ",\"native_execution\":false,\"host_compilation\":"
           << (host_compilation ? "true" : "false")
           << ",\"analysis\":{\"committed_executable_permission_bytes\":"
           << coverage.committed_executable_permission_bytes
           << ",\"static_precompiled_bytes\":" << coverage.static_precompiled_bytes
           << ",\"initially_required_bytes\":" << coverage.initially_required_bytes
           << ",\"runtime_materializable_bytes\":" << coverage.runtime_materializable_bytes
           << ",\"unknown_storage_bytes\":" << coverage.unknown_storage_bytes
           << ",\"currently_dispatchable_bytes\":" << coverage.currently_dispatchable_bytes
           << ",\"uncovered_control_targets\":" << coverage.uncovered_control_targets
           << ",\"dispatch_paths_without_validation\":"
           << coverage.dispatch_paths_without_validation
           << ",\"materialization_attempts\":" << coverage.materialization_attempts
           << ",\"materialization_successes\":" << coverage.materialization_successes
           << ",\"materialization_rejections\":" << coverage.materialization_rejections
           << ",\"materialization_budget_failures\":" << coverage.materialization_budget_failures
           << ",\"generation_revalidation_failures\":" << coverage.generation_revalidation_failures
           << ",\"byte_identity_failures\":" << coverage.byte_identity_failures
           << ",\"dispatch_validation_failures\":" << coverage.dispatch_validation_failures
           << ",\"committed_executable_bytes\":" << coverage.committed_executable_bytes
           << ",\"analyzed_instruction_bytes\":" << coverage.analyzed_instruction_bytes
           << ",\"unanalyzed_executable_bytes\":" << coverage.unanalyzed_executable_bytes
           << ",\"runtime_deferred_executable_bytes\":"
           << coverage.runtime_deferred_executable_bytes
           << ",\"never_executed_data_bytes\":" << coverage.never_executed_data_bytes
           << ",\"unknown_executable_bytes\":" << coverage.unknown_executable_bytes
           << ",\"unproven_padding_bytes\":" << coverage.unproven_padding_bytes
           << ",\"incomplete_initial_required_code_bytes\":"
           << coverage.incomplete_initial_required_code_bytes
           << ",\"uncovered_runtime_materializable_bytes\":"
           << coverage.uncovered_runtime_materializable_bytes
           << ",\"instructions\":" << coverage.instructions
           << ",\"proven_instructions\":" << coverage.proven_instructions
           << ",\"guarded_candidate_instructions\":" << coverage.guarded_candidate_instructions
           << ",\"functions\":" << coverage.functions
           << ",\"resolved_control_flow\":" << coverage.resolved_control_flow
           << ",\"guarded_control_flow\":" << coverage.guarded_control_flow
           << ",\"guarded_complete_control_flow\":" << coverage.guarded_complete_control_flow
           << ",\"guarded_partial_control_flow\":" << coverage.guarded_partial_control_flow
           << ",\"runtime_only_control_flow\":" << coverage.runtime_only_control_flow
           << ",\"unresolved_control_flow\":" << coverage.unresolved_control_flow
           << ",\"unresolved_frontier\":"
           << coverage.guarded_partial_control_flow + coverage.runtime_only_control_flow +
                  coverage.unresolved_control_flow
           << ",\"unknown_instructions\":" << coverage.unknown_instructions
           << ",\"reachable_abort_edges\":" << coverage.reachable_abort_edges
           << ",\"executable_byte_classes\":{";
    for (std::size_t current = 0u; current < coverage.executable_byte_classes.size(); ++current) {
        if (current != 0u) output << ',';
        output << io::quote_json(analysis::executable_byte_class_name(
                      static_cast<analysis::ExecutableByteClass>(current)))
               << ':' << coverage.executable_byte_classes[current];
    }
    output << "},\"precompile_sets\":{";
    for (std::size_t current = 0u; current < coverage.precompile_classes.size(); ++current) {
        if (current != 0u) output << ',';
        output << io::quote_json(analysis::precompile_class_name(
                      static_cast<analysis::PrecompileClass>(current)))
               << ':' << coverage.precompile_classes[current];
    }
    output << "},\"mixed_range_roles\":{";
    for (std::size_t current = 0u; current < coverage.mixed_range_roles.size(); ++current) {
        if (current != 0u) output << ',';
        output << io::quote_json(analysis::mixed_range_role_name(
                      static_cast<analysis::MixedRangeRole>(current)))
               << ':' << coverage.mixed_range_roles[current];
    }
    output << "},\"range_proof_classes\":{";
    for (std::size_t current = 0u; current < coverage.range_proof_classes.size(); ++current) {
        if (current != 0u) output << ',';
        output << io::quote_json(analysis::range_proof_class_name(
                      static_cast<analysis::RangeProofClass>(current)))
               << ':' << coverage.range_proof_classes[current];
    }
    output << '}'
           << ",\"control_flow_complete\":" << (coverage.control_flow_complete ? "true" : "false")
           << "}}\n";
    return output.str();
}

std::string diagnostic_json(const Diagnostic& diagnostic) {
    std::ostringstream output;
    const auto severity = diagnostic.severity == DiagnosticSeverity::Error     ? "error"
                          : diagnostic.severity == DiagnosticSeverity::Warning ? "warning"
                                                                               : "information";
    output << "{\"severity\":" << io::quote_json(severity)
           << ",\"code\":" << io::quote_json(diagnostic.code)
           << ",\"message\":" << io::quote_json(diagnostic.message)
           << ",\"recovery\":" << io::quote_json(diagnostic.recovery);
    if (diagnostic.source_line) output << ",\"source_line\":" << *diagnostic.source_line;
    output << '}';
    return output.str();
}

std::vector<std::string> split_lines(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
        lines.push_back(line);
    return lines;
}

std::uint32_t parse_decimal(const std::string_view text, const char* field) {
    std::uint32_t value = 0u;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::runtime_error(std::string("Ungueltiges Einstellungsfeld ") + field + '.');
    return value;
}

void require_firmware_profile(const io::ProjectManifest& profile,
                              const std::optional<std::filesystem::path>& port_output_directory) {
    platform::FirmwareMode firmware_mode = platform::FirmwareMode::DirectHomebrew;
    switch (profile.firmware_mode) {
    case io::ProjectFirmwareMode::Direct:
        break;
    case io::ProjectFirmwareMode::Hle:
        firmware_mode = platform::FirmwareMode::HleBiosAbi;
        break;
    case io::ProjectFirmwareMode::Lle:
        firmware_mode = platform::FirmwareMode::LleFirmware;
        break;
    }
    platform::AlphaFirmwareInputPolicy firmware_inputs;
    firmware_inputs.bios_source = profile.bios_path;
    firmware_inputs.flash_source = profile.flash_path;
    firmware_inputs.port_output_directory = port_output_directory;
    platform::require_alpha_firmware_profile(firmware_mode, firmware_inputs);
}

} // namespace

void require_cpp_profile_capabilities(
    const io::ProjectManifest& profile,
    const std::optional<std::filesystem::path>& port_output_directory) {
    require_firmware_profile(profile, port_output_directory);

    std::vector<std::string> unsupported;
    for (const auto& capability_name : profile.required_backend_capabilities) {
        if (capability_name != "memory" && capability_name != "firmware-mode")
            unsupported.push_back(capability_name);
    }
    if (profile.fallback_policy != io::ProjectFallbackPolicy::Abort)
        unsupported.push_back("fallback-profile");
    if (profile.mmu_profile == io::ProjectMmuProfile::Sh4) unsupported.push_back("mmu");
    if (profile.fastpath_profile != io::ProjectFastpathProfile::Conservative)
        unsupported.push_back("fastpath-profile");
    if (!profile.alias_groups.empty() || !profile.canonical_physical_ranges.empty())
        unsupported.push_back("address-mapping-profile");
    if (!profile.writable_executable_ranges.empty())
        unsupported.push_back("executable-ram-profile");
    if (profile.bios_path || profile.flash_path || !profile.dynamic_bios_vectors.empty())
        unsupported.push_back("firmware-image-profile");
    std::sort(unsupported.begin(), unsupported.end());
    unsupported.erase(std::unique(unsupported.begin(), unsupported.end()), unsupported.end());
    if (unsupported.empty()) return;
    std::ostringstream message;
    message << "Das C++-Backend kann folgende Manifest-Faehigkeiten nicht anwenden: ";
    for (std::size_t index = 0u; index < unsupported.size(); ++index) {
        if (index != 0u) message << ", ";
        message << unsupported[index];
    }
    throw std::runtime_error(message.str() + '.');
}

ProjectSession::ProjectSession(std::filesystem::path path,
                               io::ProjectManifest manifest,
                               const bool dirty)
    : path_(std::move(path)), manifest_(std::move(manifest)), dirty_(dirty) {}

ProjectSession ProjectSession::create(std::filesystem::path manifest_path,
                                      io::ProjectManifest manifest) {
    if (manifest_path.empty()) throw std::invalid_argument("Projektpfad fehlt.");
    return ProjectSession(std::move(manifest_path), std::move(manifest), true);
}

ProjectSession ProjectSession::open(const std::filesystem::path& manifest_path) {
    return ProjectSession(manifest_path, io::parse_project_manifest(manifest_path), false);
}

const std::filesystem::path& ProjectSession::path() const noexcept {
    return path_;
}
const io::ProjectManifest& ProjectSession::manifest() const noexcept {
    return manifest_;
}
io::ProjectManifest& ProjectSession::edit() noexcept {
    dirty_ = true;
    ++revision_;
    return manifest_;
}
bool ProjectSession::dirty() const noexcept {
    return dirty_;
}
std::uint64_t ProjectSession::revision() const noexcept {
    return revision_;
}

void ProjectSession::save() {
    const auto content = io::serialize_project_manifest(manifest_, path_);
    auto validation_path = path_;
    validation_path += ".katana-validate";
    write_atomic(validation_path, content);
    try {
        static_cast<void>(io::parse_project_manifest(validation_path));
    } catch (...) {
        std::filesystem::remove(validation_path);
        throw;
    }
    std::error_code error;
    std::filesystem::rename(validation_path, path_, error);
    if (error) {
        std::filesystem::remove(path_, error);
        error.clear();
        std::filesystem::rename(validation_path, path_, error);
    }
    if (error) {
        std::filesystem::remove(validation_path);
        throw std::runtime_error("Projektmanifest konnte nicht atomar gespeichert werden.");
    }
    manifest_ = io::parse_project_manifest(path_);
    dirty_ = false;
}

UserSettings load_user_settings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    UserSettings settings;
    const auto lines = split_lines(read_text(path));
    for (const auto& line : lines) {
        if (line.empty() || line.starts_with('#')) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            throw std::runtime_error("Einstellungsdatei ist ungueltig.");
        const auto key = line.substr(0u, separator);
        const auto value = line.substr(separator + 1u);
        if (key == "version")
            settings.version = parse_decimal(value, "version");
        else if (key == "theme")
            settings.theme = value;
        else if (key == "scale_percent")
            settings.scale_percent = parse_decimal(value, "scale_percent");
        else if (key == "restore_last_project")
            settings.restore_last_project = value == "true";
        else if (key == "recent")
            settings.recent_projects.emplace_back(value);
        else
            throw std::runtime_error("Unbekanntes Einstellungsfeld.");
    }
    if (settings.version != settings_schema_version)
        throw std::runtime_error("Einstellungsversion wird nicht unterstuetzt.");
    if (settings.theme != "system" && settings.theme != "light" && settings.theme != "dark")
        throw std::runtime_error("Unbekanntes Darstellungsthema.");
    if (settings.scale_percent < 75u || settings.scale_percent > 300u)
        throw std::runtime_error("GUI-Skalierung liegt ausserhalb 75 bis 300 Prozent.");
    if (settings.recent_projects.size() > 10u) settings.recent_projects.resize(10u);
    return settings;
}

void save_user_settings(const std::filesystem::path& path, const UserSettings& settings) {
    if (settings.version != settings_schema_version ||
        (settings.theme != "system" && settings.theme != "light" && settings.theme != "dark") ||
        settings.scale_percent < 75u || settings.scale_percent > 300u) {
        throw std::invalid_argument("GUI-Einstellungen sind ungueltig.");
    }
    std::ostringstream output;
    output << "version=" << settings_schema_version << "\ntheme=" << settings.theme
           << "\nscale_percent=" << settings.scale_percent
           << "\nrestore_last_project=" << (settings.restore_last_project ? "true" : "false")
           << '\n';
    const auto count = std::min<std::size_t>(10u, settings.recent_projects.size());
    for (std::size_t index = 0u; index < count; ++index)
        output << "recent=" << settings.recent_projects[index].generic_string() << '\n';
    write_atomic(path, output.str());
}

void remember_recent_project(UserSettings& settings, const std::filesystem::path& path) {
    const auto normalized = std::filesystem::absolute(path).lexically_normal();
    settings.recent_projects.erase(
        std::remove(settings.recent_projects.begin(), settings.recent_projects.end(), normalized),
        settings.recent_projects.end());
    settings.recent_projects.insert(settings.recent_projects.begin(), normalized);
    if (settings.recent_projects.size() > 10u) settings.recent_projects.resize(10u);
}

void Cancellation::request() noexcept {
    requested_.store(true);
}
bool Cancellation::requested() const noexcept {
    return requested_.load();
}

ApplicationService::ApplicationService(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

SourceInspection ApplicationService::inspect_source(const io::ProjectManifest& manifest) const {
    return capture_project_snapshot(manifest).inspection;
}

JobResult ApplicationService::execute(const JobRequest& request,
                                      const std::shared_ptr<Cancellation>& cancellation,
                                      const JobObserver& observer) const {
    require_stable_id(request.id, "Job-ID");
    require_stable_id(request.tool_version, "Werkzeugversion");
    if (request.manifest_path.empty() || request.output_root.empty())
        throw std::invalid_argument("Job braucht Projektmanifest und Ausgabeziel.");
    CrossProcessJobLock process_lock(normalized_output_path(request.output_root));
    JobResult result;
    result.job_id = request.id;
    result.kind = request.kind;
    result.state = JobState::Running;
    result.tool_version = request.tool_version;
    result.failure_category = JobFailureCategory::InputOutput;
    const bool transactional =
        request.kind == JobKind::Build || request.kind == JobKind::RunPreflight;
    const auto final_root = std::filesystem::absolute(request.output_root).lexically_normal();
    auto work_root = final_root;
    auto stale_root = final_root;
    stale_root += ".katana-stale-" + request.id;
    if (transactional) {
        const auto staging_key = io::sha256_bytes(final_root.generic_string() + ':' + request.id);
        work_root = final_root.parent_path() / (".katana-stage-" + staging_key.substr(0u, 12u));
        std::error_code cleanup_error;
        std::filesystem::remove_all(work_root, cleanup_error);
        if (cleanup_error)
            throw std::runtime_error("Altes Job-Staging konnte nicht entfernt werden.");
        if (std::filesystem::exists(final_root)) {
            if (std::filesystem::exists(stale_root)) {
                std::filesystem::remove_all(final_root, cleanup_error);
                if (cleanup_error)
                    throw std::runtime_error(
                        "Vorheriger Fehlerbericht konnte nicht ersetzt werden.");
            } else {
                std::filesystem::rename(final_root, stale_root);
            }
        }
        std::filesystem::create_directories(work_root);
    }
    JobEventStream events(request, observer);
    events.emit(JobState::Queued, 0u, "queued", {}, JobStepStatus::Pending);
    events.emit(JobState::Running, 2u, "validation", {}, JobStepStatus::Running);
    try {
        require_not_cancelled(cancellation);
        auto manifest = io::parse_project_manifest(request.manifest_path);
        require_firmware_profile(manifest, final_root);
        events.emit(JobState::Running, 5u, "validation", {}, JobStepStatus::Completed, 1u, 1u);
        events.emit(JobState::Running, 5u, "hashing", {}, JobStepStatus::Running);
        const auto snapshot = capture_project_snapshot(manifest, cancellation);
        events.emit(JobState::Running,
                    12u,
                    "hashing",
                    {},
                    JobStepStatus::Completed,
                    snapshot.inputs.size(),
                    snapshot.inputs.size());
        events.emit(JobState::Running, 12u, "boot-image", {}, JobStepStatus::Running);
        require_not_cancelled(cancellation);
        const auto project = io::load_project(std::move(manifest));
        if (!same_snapshot(snapshot,
                           capture_project_snapshot(project.execution_profile, cancellation))) {
            throw std::runtime_error(
                "Eine wirksame Projekteingabe wurde zwischen Snapshot und Laden veraendert.");
        }
        const auto& inspection = snapshot.inspection;
        result.project_identity = project_identity(project.execution_profile, snapshot);
        result.failure_category = JobFailureCategory::Processing;
        result.checkpoints.push_back("project-validated");
        events.emit(JobState::Running, 20u, "boot-image", {}, JobStepStatus::Completed, 1u, 1u);
        require_not_cancelled(cancellation);
        if (request.kind == JobKind::Validate) {
            const auto report_path = work_root / "source-inspection.json";
            write_atomic(report_path, format_source_inspection_json(inspection));
            result.artifacts.push_back(
                {"source-inspection", "source-inspection.json", artifact_hash(report_path)});
        } else {
            std::optional<analysis::AnalysisOverrides> overrides;
            if (project.execution_profile.analysis_overrides_path) {
                overrides = analysis::parse_analysis_overrides(
                    *project.execution_profile.analysis_overrides_path);
            }
            events.emit(JobState::Running, 20u, "analysis", {}, JobStepStatus::Running);
            const auto analysis =
                analysis::analyze_control_flow(project.image, overrides ? &*overrides : nullptr);
            const auto executable_inventory =
                analysis::build_executable_byte_inventory(project.image, analysis);
            result.analysis_coverage = analysis_coverage(project, analysis, executable_inventory);
            result.checkpoints.push_back("analysis-complete");
            events.emit(JobState::Running,
                        45u,
                        "analysis",
                        {},
                        JobStepStatus::Completed,
                        result.analysis_coverage->analyzed_instruction_bytes,
                        result.analysis_coverage->committed_executable_bytes);
            require_not_cancelled(cancellation);
            const auto analysis_json = analysis::format_control_flow_analysis_json(analysis);
            const auto analysis_path = work_root / "analysis.json";
            write_atomic(analysis_path, analysis_json);
            result.artifacts.push_back({"analysis", "analysis.json", artifact_hash(analysis_path)});
            const auto inventory_path = work_root / "executable-inventory.json";
            write_atomic(inventory_path,
                         analysis::format_executable_inventory_json(
                             project.image, executable_inventory, false));
            result.artifacts.push_back({"executable-inventory",
                                        "executable-inventory.json",
                                        artifact_hash(inventory_path)});
            const auto local_inventory_path = work_root / "executable-inventory.local.json";
            write_atomic(local_inventory_path,
                         analysis::format_executable_inventory_json(
                             project.image, executable_inventory, true));
            result.artifacts.push_back({"executable-inventory-local",
                                        "executable-inventory.local.json",
                                        artifact_hash(local_inventory_path)});
            const auto frontier_path = work_root / "control-flow-frontier.json";
            write_atomic(frontier_path, analysis::format_control_flow_frontier_json(analysis));
            result.artifacts.push_back({"control-flow-frontier",
                                        "control-flow-frontier.json",
                                        artifact_hash(frontier_path)});
            const auto result_index_path = work_root / "result-index.json";
            write_atomic(result_index_path,
                         result_index_json(project, analysis, inspection, result.project_identity));
            result.artifacts.push_back(
                {"result-index", "result-index.json", artifact_hash(result_index_path)});
            for (const auto& diagnostic : analysis.recursive.diagnostics) {
                result.diagnostics.push_back(
                    {DiagnosticSeverity::Warning,
                     "analysis-unknown-instruction",
                     "Unbekannte Instruktion bei " + hex_address(diagnostic.address) + ": " +
                         redact_sensitive_text(diagnostic.reason),
                     "Analyseprofil, Override oder unterstuetzte ISA-Abdeckung pruefen.",
                     std::nullopt});
            }
            if (request.kind == JobKind::Codegen || request.kind == JobKind::Build ||
                request.kind == JobKind::RunPreflight) {
                result.failure_category = JobFailureCategory::CodeGeneration;
                require_cpp_profile_capabilities(project.execution_profile);
            }
            if (!result.analysis_coverage->control_flow_complete) {
                result.diagnostics.push_back(
                    {DiagnosticSeverity::Warning,
                     "analysis-incomplete",
                     "Kontrollflussanalyse ist unvollstaendig: " +
                         std::to_string(result.analysis_coverage->guarded_partial_control_flow) +
                         " partielle, " +
                         std::to_string(result.analysis_coverage->unresolved_control_flow) +
                         " ungeloeste Kontrollflussstellen, " +
                         std::to_string(result.analysis_coverage->unknown_instructions) +
                         " unbekannte Instruktionen; " +
                         std::to_string(result.analysis_coverage->uncovered_control_targets) +
                         " Kontrollflussziele sind nicht abgedeckt, " +
                         std::to_string(
                             result.analysis_coverage->dispatch_paths_without_validation) +
                         " Dispatchpfade umgehen die Validierung und " +
                         std::to_string(result.analysis_coverage->reachable_abort_edges) +
                         " erreichbare Kanten brechen die Analyse ab. " +
                         std::to_string(result.analysis_coverage->runtime_only_control_flow) +
                         " reine Laufzeitstellen sind separat validierend abgedeckt.",
                     "Kontrollfluss- oder Dispatchblocker beheben; unbekannte Speicherbytes "
                     "allein blockieren keinen Hostbuild.",
                     std::nullopt});
                if (request.kind == JobKind::Build || request.kind == JobKind::RunPreflight) {
                    const auto build_report = work_root / "build-plan.json";
                    write_atomic(
                        build_report,
                        build_plan_json(
                            "partial", request.tool_version, *result.analysis_coverage, false));
                    result.artifacts.push_back(
                        {"build-plan", "build-plan.json", artifact_hash(build_report)});
                }
            } else if (request.kind != JobKind::Analyze) {
                events.emit(JobState::Running, 45u, "ir", {}, JobStepStatus::Running);
                auto program = ir::lower_program(analysis);
                static_cast<void>(ir::optimize_program(program));
                ir::require_valid_program(program);
                events.emit(JobState::Running,
                            55u,
                            "ir",
                            {},
                            JobStepStatus::Completed,
                            program.size(),
                            program.size());
                const auto entry = project.execution_profile.entry_point.value_or(
                    project.image.entry_points().empty() ? 0u
                                                         : project.image.entry_points().front());
                if (entry == 0u)
                    throw std::runtime_error("Projekt besitzt keinen Codegen-Einstieg.");
                events.emit(JobState::Running, 55u, "codegen", {}, JobStepStatus::Running);
                const auto source = codegen::emit_cpp_program(program, entry);
                result.checkpoints.push_back("codegen-complete");
                events.emit(
                    JobState::Running, 70u, "codegen", {}, JobStepStatus::Completed, 1u, 1u);
                require_not_cancelled(cancellation);
                const bool gdi_host_build =
                    (request.kind == JobKind::Build || request.kind == JobKind::RunPreflight) &&
                    project.execution_profile.format == io::ProjectInputFormat::DreamcastGdi;
                if (!gdi_host_build) {
                    const auto write = codegen::write_codegen_project(work_root / "generated",
                                                                      {{"program.cpp", source}});
                    for (const auto& relative : write.written_files) {
                        const auto path = work_root / "generated" / relative;
                        result.artifacts.push_back({"generated",
                                                    std::filesystem::path("generated") / relative,
                                                    artifact_hash(path)});
                    }
                }
                if (request.kind == JobKind::Build || request.kind == JobKind::RunPreflight) {
                    const auto runtime_root =
                        runtime_root_.empty() ? discover_runtime_root() : runtime_root_;
                    const auto host_log = work_root / "recompile.log";
                    result.failure_category = JobFailureCategory::Build;
                    if (project.execution_profile.format == io::ProjectInputFormat::DreamcastGdi) {
                        const auto host_root = work_root / "sourcecode";
                        const auto boot_size = project.image.segments().empty()
                                                   ? 0u
                                                   : project.image.segments().front().bytes.size();
                        const auto port_export = codegen::export_dreamcast_port_project(
                            {project.image,
                             analysis,
                             program,
                             snapshot.inputs,
                             entry,
                             platform::dreamcast_disc_boot_address,
                             boot_size,
                             result.project_identity,
                             project.execution_profile.firmware_mode ==
                                 io::ProjectFirmwareMode::Hle},
                            host_root,
                            {"game", request.tool_version, {}, {}});
                        require_not_cancelled(cancellation);
                        const auto host_build_root = work_root / ".katana-build";
                        events.emit(JobState::Running,
                                    72u,
                                    "host-configuration",
                                    {},
                                    JobStepStatus::Running);
                        configure_and_build(host_root,
                                            host_build_root,
                                            runtime_root,
                                            "game",
                                            host_log,
                                            cancellation,
                                            events);
                        events.emit(JobState::Running,
                                    95u,
                                    "host-compilation",
                                    {},
                                    JobStepStatus::Completed);
                        auto executable = host_build_root /
#ifdef _WIN32
                                          "Debug" / "game.exe";
                        if (!std::filesystem::exists(executable))
                            executable = host_build_root / "game.exe";
#else
                                          "game";
#endif
                        if (!std::filesystem::is_regular_file(executable))
                            throw std::runtime_error(
                                "Hostbuild hat kein aktuelles ausfuehrbares Artefakt erzeugt.");
                        const auto published_executable = work_root / executable.filename();
                        std::filesystem::copy_file(
                            executable,
                            published_executable,
                            std::filesystem::copy_options::overwrite_existing);
                        const auto executable_sha256 = artifact_hash(published_executable);
                        const auto content_root = work_root / "content";
                        std::filesystem::create_directories(content_root);
                        const auto published_pack = content_root / "game.katana-disc";
                        std::filesystem::copy_file(
                            port_export.packed_disc,
                            published_pack,
                            std::filesystem::copy_options::overwrite_existing);
                        auto packed = runtime::PackedDiscSource::open(published_pack);
                        packed->verify_all_chunks();
                        auto packed_info = packed->info();
                        packed.reset();
                        const auto packed_sha256 = artifact_hash(published_pack);
                        const auto published_manifest = content_root / "game.katana-disc.json";
                        write_atomic(published_manifest,
                                     runtime::format_packed_disc_manifest(
                                         packed_info,
                                         packed_sha256,
                                         executable_sha256,
                                         (std::filesystem::path("..") / executable.filename())
                                             .generic_string(),
                                         std::filesystem::file_size(published_executable)));
                        const auto package_runtime_root = work_root / "runtime";
                        std::filesystem::create_directories(package_runtime_root);
                        const auto runtime_manifest =
                            package_runtime_root / "runtime-dependencies.json";
                        write_atomic(runtime_manifest,
                                     "{\"schema\":\"katana-runtime-dependencies\",\"version\":1,"
                                     "\"linkage\":\"static\",\"job_generation\":\"" +
                                         result.project_identity + "\",\"files\":[]}\n");
                        std::filesystem::create_directories(work_root / "user-data");
                        result.artifacts.push_back(
                            {"host_executable",
                             executable.filename(),
                             executable_sha256,
                             std::filesystem::file_size(published_executable),
                             1u,
                             result.project_identity});
                        result.artifacts.push_back(
                            {"packed_disc",
                             std::filesystem::path("content") / "game.katana-disc",
                             packed_sha256,
                             std::filesystem::file_size(published_pack),
                             runtime::packed_disc_format_version,
                             result.project_identity});
                        result.artifacts.push_back(
                            {"packed_disc_manifest",
                             std::filesystem::path("content") / "game.katana-disc.json",
                             artifact_hash(published_manifest),
                             std::filesystem::file_size(published_manifest),
                             1u,
                             result.project_identity});
                        result.artifacts.push_back(
                            {"runtime_dependencies",
                             std::filesystem::path("runtime") / "runtime-dependencies.json",
                             artifact_hash(runtime_manifest),
                             std::filesystem::file_size(runtime_manifest),
                             1u,
                             result.project_identity});
                        std::filesystem::remove_all(host_root / "content");
                        std::filesystem::remove_all(host_root / "runtime");
                        std::filesystem::remove_all(host_root / "user-data");
                        std::error_code cleanup_error;
                        std::filesystem::remove_all(host_build_root, cleanup_error);
                        if (cleanup_error)
                            throw std::runtime_error(
                                "Temporaeres Hostbuildverzeichnis konnte nicht entfernt werden.");
                    } else {
                        const auto generated_root = work_root / "generated";
                        events.emit(JobState::Running,
                                    72u,
                                    "host-configuration",
                                    {},
                                    JobStepStatus::Running);
                        configure_and_build(generated_root,
                                            work_root / "host-build",
                                            runtime_root,
                                            "katana_generated",
                                            host_log,
                                            cancellation,
                                            events);
                        events.emit(JobState::Running,
                                    95u,
                                    "host-compilation",
                                    {},
                                    JobStepStatus::Completed);
                    }
                    result.artifacts.push_back(
                        {"recompile-log", "recompile.log", artifact_hash(host_log)});
                    require_not_cancelled(cancellation);
                    result.checkpoints.push_back("host-build-complete");
                    const auto build_report = work_root / "build-plan.json";
                    write_atomic(
                        build_report,
                        build_plan_json(
                            "built", request.tool_version, *result.analysis_coverage, true));
                    result.artifacts.push_back(
                        {"build-plan", "build-plan.json", artifact_hash(build_report)});
                }
                if (request.kind == JobKind::RunPreflight) {
                    result.checkpoints.push_back("run-preflight-ready");
                    result.diagnostics.push_back(
                        {DiagnosticSeverity::Information,
                         "native-run-deferred",
                         "Der gemeinsame Run-Job hat Analyse, Codegen und Hostbuild ausgefuehrt.",
                         "Native Hostausfuehrung wird im Phase-11-Runtime-Scope aktiviert.",
                         std::nullopt});
                }
            }
        }
        require_not_cancelled(cancellation);
        result.failure_category = JobFailureCategory::Processing;
        events.emit(JobState::Running, 96u, "finalization", {}, JobStepStatus::Running);
        if (!same_snapshot(snapshot,
                           capture_project_snapshot(project.execution_profile, cancellation))) {
            throw std::runtime_error(
                "Eine wirksame Projekteingabe wurde waehrend des Jobs veraendert.");
        }
        result.state = result.analysis_coverage && !result.analysis_coverage->control_flow_complete
                           ? JobState::Partial
                           : JobState::Completed;
        result.failure_category = JobFailureCategory::None;
    } catch (const JobState state) {
        result.state = state;
        result.failure_category = JobFailureCategory::None;
        result.diagnostics.push_back(
            make_error("job-cancelled",
                       "Job wurde kontrolliert abgebrochen.",
                       "Der Job kann mit denselben Eingaben wiederholt werden."));
    } catch (const io::InputOutputError& error) {
        result.state = JobState::Failed;
        result.failure_category = JobFailureCategory::InputOutput;
        result.diagnostics.push_back(
            make_error("job-input-output-failed",
                       error.what(),
                       "Eingabe- und Ausgabepfade sowie Zugriffsrechte pruefen."));
    } catch (const std::filesystem::filesystem_error& error) {
        result.state = JobState::Failed;
        result.failure_category = JobFailureCategory::InputOutput;
        result.diagnostics.push_back(
            make_error("job-input-output-failed",
                       error.what(),
                       "Eingabe- und Ausgabepfade sowie Zugriffsrechte pruefen."));
    } catch (const std::exception& error) {
        result.state = JobState::Failed;
        result.diagnostics.push_back(
            make_error("job-failed",
                       error.what(),
                       "Quelle und Projekteinstellungen pruefen und Job wiederholen."));
    }
    auto result_path = work_root / "job-result.json";
    try {
        if (transactional && result.state != JobState::Completed &&
            result.state != JobState::Partial)
            result.artifacts.clear();
        write_atomic(result_path, format_job_result_json(result));
        if (transactional) {
            if (result.state == JobState::Completed || result.state == JobState::Partial) {
                std::filesystem::rename(work_root, final_root);
                std::error_code cleanup_error;
                std::filesystem::remove_all(stale_root, cleanup_error);
                if (cleanup_error)
                    throw std::runtime_error(
                        "Veraltetes Jobergebnis konnte nicht bereinigt werden.");
            } else {
                std::error_code cleanup_error;
                std::filesystem::remove_all(work_root, cleanup_error);
                if (cleanup_error)
                    throw std::runtime_error(
                        "Fehlgeschlagenes Job-Staging konnte nicht bereinigt werden.");
                std::filesystem::create_directories(final_root);
                result_path = final_root / "job-result.json";
                write_atomic(result_path, format_job_result_json(result));
            }
            result_path = final_root / "job-result.json";
        }
        result.artifacts.push_back({"job-result", "job-result.json", artifact_hash(result_path)});
    } catch (const std::exception& error) {
        result.state = JobState::Failed;
        result.failure_category = JobFailureCategory::InputOutput;
        result.artifacts.clear();
        result.diagnostics.push_back(
            make_error("job-publication-failed",
                       error.what(),
                       "Ausgabeziel, Zugriffsrechte und freien Speicher pruefen."));
        if (transactional) {
            std::error_code ignored;
            std::filesystem::remove_all(work_root, ignored);
            if (std::filesystem::exists(final_root)) {
                result_path = final_root / "job-result.json";
                try {
                    write_atomic(result_path, format_job_result_json(result));
                    result.artifacts.push_back(
                        {"job-result", "job-result.json", artifact_hash(result_path)});
                } catch (...) {
                }
            }
        }
    }
    const auto terminal_status = result.state == JobState::Cancelled ? JobStepStatus::Cancelled
                                 : result.state == JobState::Failed  ? JobStepStatus::Failed
                                                                     : JobStepStatus::Completed;
    const auto terminal_diagnostic =
        result.diagnostics.empty() ? std::optional<Diagnostic>{} : result.diagnostics.back();
    events.emit(result.state, 100u, "finalization", terminal_diagnostic, terminal_status, 1u, 1u);
    return result;
}

JobCoordinator::JobCoordinator(ApplicationService service) : service_(std::move(service)) {}

JobResult JobCoordinator::execute(const JobRequest& request,
                                  const std::shared_ptr<Cancellation>& cancellation,
                                  const JobObserver& observer) {
    const auto output = normalized_output_path(request.output_root);
    {
        std::scoped_lock lock(mutex_);
        if (std::any_of(active_outputs_.begin(), active_outputs_.end(), [&](const auto& active) {
                return outputs_overlap(active, output);
            }))
            throw std::runtime_error(
                "Ein aktiver Job verwendet bereits ein ueberlappendes Ausgabeziel.");
        active_outputs_.push_back(output);
    }
    try {
        auto result = service_.execute(request, cancellation, observer);
        std::scoped_lock lock(mutex_);
        std::erase(active_outputs_, output);
        return result;
    } catch (...) {
        std::scoped_lock lock(mutex_);
        std::erase(active_outputs_, output);
        throw;
    }
}

const char* job_kind_name(const JobKind kind) noexcept {
    switch (kind) {
    case JobKind::Validate:
        return "validate";
    case JobKind::Analyze:
        return "analyze";
    case JobKind::Codegen:
        return "codegen";
    case JobKind::Build:
        return "build";
    case JobKind::RunPreflight:
        return "run-preflight";
    }
    return "unknown";
}

const char* job_state_name(const JobState state) noexcept {
    switch (state) {
    case JobState::Queued:
        return "queued";
    case JobState::Running:
        return "running";
    case JobState::Completed:
        return "completed";
    case JobState::Partial:
        return "partial";
    case JobState::Failed:
        return "failed";
    case JobState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

const char* job_step_status_name(const JobStepStatus status) noexcept {
    switch (status) {
    case JobStepStatus::Pending:
        return "pending";
    case JobStepStatus::Running:
        return "running";
    case JobStepStatus::Completed:
        return "completed";
    case JobStepStatus::Failed:
        return "failed";
    case JobStepStatus::Cancelled:
        return "cancelled";
    case JobStepStatus::Skipped:
        return "skipped";
    }
    return "unknown";
}

const char* job_failure_category_name(const JobFailureCategory category) noexcept {
    switch (category) {
    case JobFailureCategory::None:
        return "none";
    case JobFailureCategory::InputOutput:
        return "input-output";
    case JobFailureCategory::Processing:
        return "processing";
    case JobFailureCategory::CodeGeneration:
        return "code-generation";
    case JobFailureCategory::Build:
        return "build";
    case JobFailureCategory::Internal:
        return "internal";
    }
    return "internal";
}

std::string redact_sensitive_text(const std::string_view text) {
    std::string result(text);
    for (std::size_t index = 0u; index < result.size();) {
        const bool drive = index + 2u < result.size() &&
                           ((result[index] >= 'A' && result[index] <= 'Z') ||
                            (result[index] >= 'a' && result[index] <= 'z')) &&
                           result[index + 1u] == ':' &&
                           (result[index + 2u] == '\\' || result[index + 2u] == '/');
        const bool path_boundary = index == 0u || result[index - 1u] == ' ' ||
                                   result[index - 1u] == '\t' || result[index - 1u] == '"' ||
                                   result[index - 1u] == '\'' || result[index - 1u] == '(' ||
                                   result[index - 1u] == '=' || result[index - 1u] == ':';
        auto end = index;
        while (end < result.size() && result[end] != '\n' && result[end] != '\r' &&
               result[end] != ' ' && result[end] != '\t' && result[end] != '"' &&
               result[end] != '\'' && result[end] != ')' && result[end] != ',' &&
               result[end] != ';')
            ++end;
        const bool absolute_posix = result[index] == '/' && path_boundary &&
                                    index + 1u < result.size() && result[index + 1u] != '/' &&
                                    result.find('/', index + 1u) < end;
        if (!drive && !absolute_posix) {
            ++index;
            continue;
        }
        const bool quoted =
            index != 0u && (result[index - 1u] == '"' || result[index - 1u] == '\'');
        if (drive && quoted) {
            end = index;
            const auto quote = result[index - 1u];
            while (end < result.size() && result[end] != '\n' && result[end] != '\r' &&
                   result[end] != quote)
                ++end;
        }
        result.replace(index, end - index, "<redacted-path>");
        index += 15u;
    }
    static constexpr std::string_view forbidden[] = {
        "firmware_bytes", "flash_bytes", "bios_bytes", "serial_number"};
    for (const auto token : forbidden) {
        std::size_t position = 0u;
        while ((position = result.find(token, position)) != std::string::npos) {
            result.replace(position, token.size(), "redacted_field");
            position += 14u;
        }
    }
    return result;
}

std::string format_source_inspection_json(const SourceInspection& inspection) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-source-inspection\",\"version\":1,\"format\":"
           << io::quote_json(inspection.format)
           << ",\"display_name\":" << io::quote_json(inspection.display_name)
           << ",\"size\":" << inspection.size << ",\"sha256\":" << io::quote_json(inspection.sha256)
           << ",\"read_only\":" << (inspection.read_only ? "true" : "false") << ",\"tracks\":[";
    for (std::size_t index = 0u; index < inspection.tracks.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& track = inspection.tracks[index];
        output << "{\"number\":" << track.number << ",\"lba\":" << track.lba
               << ",\"role\":" << io::quote_json(track.role)
               << ",\"sector_size\":" << track.sector_size << ",\"file_size\":" << track.file_size
               << ",\"file_offset\":" << track.file_offset
               << ",\"sector_count\":" << track.sector_count
               << ",\"descriptor_line\":" << track.descriptor_line
               << ",\"file_name\":" << io::quote_json(track.file_name)
               << ",\"sha256\":" << io::quote_json(track.sha256) << '}';
    }
    output << "],\"diagnostics\":[";
    for (std::size_t index = 0u; index < inspection.diagnostics.size(); ++index) {
        if (index != 0u) output << ',';
        output << diagnostic_json(inspection.diagnostics[index]);
    }
    output << "]}\n";
    return output.str();
}

std::string format_job_result_json(const JobResult& result) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-application-job\",\"version\":" << application_contract_version
           << ",\"job_id\":" << io::quote_json(result.job_id)
           << ",\"kind\":" << io::quote_json(job_kind_name(result.kind))
           << ",\"state\":" << io::quote_json(job_state_name(result.state))
           << ",\"failure_category\":"
           << io::quote_json(job_failure_category_name(result.failure_category))
           << ",\"tool_version\":" << io::quote_json(result.tool_version)
           << ",\"project_identity\":" << io::quote_json(result.project_identity)
           << ",\"analysis\":";
    if (!result.analysis_coverage) {
        output << "null";
    } else {
        const auto& coverage = *result.analysis_coverage;
        output << "{\"committed_executable_permission_bytes\":"
               << coverage.committed_executable_permission_bytes
               << ",\"static_precompiled_bytes\":" << coverage.static_precompiled_bytes
               << ",\"initially_required_bytes\":" << coverage.initially_required_bytes
               << ",\"runtime_materializable_bytes\":" << coverage.runtime_materializable_bytes
               << ",\"unknown_storage_bytes\":" << coverage.unknown_storage_bytes
               << ",\"currently_dispatchable_bytes\":" << coverage.currently_dispatchable_bytes
               << ",\"uncovered_control_targets\":" << coverage.uncovered_control_targets
               << ",\"dispatch_paths_without_validation\":"
               << coverage.dispatch_paths_without_validation
               << ",\"materialization_attempts\":" << coverage.materialization_attempts
               << ",\"materialization_successes\":" << coverage.materialization_successes
               << ",\"materialization_rejections\":" << coverage.materialization_rejections
               << ",\"materialization_budget_failures\":"
               << coverage.materialization_budget_failures
               << ",\"generation_revalidation_failures\":"
               << coverage.generation_revalidation_failures
               << ",\"byte_identity_failures\":" << coverage.byte_identity_failures
               << ",\"dispatch_validation_failures\":" << coverage.dispatch_validation_failures
               << ",\"committed_executable_bytes\":" << coverage.committed_executable_bytes
               << ",\"analyzed_instruction_bytes\":" << coverage.analyzed_instruction_bytes
               << ",\"unanalyzed_executable_bytes\":" << coverage.unanalyzed_executable_bytes
               << ",\"runtime_deferred_executable_bytes\":"
               << coverage.runtime_deferred_executable_bytes
               << ",\"never_executed_data_bytes\":" << coverage.never_executed_data_bytes
               << ",\"unknown_executable_bytes\":" << coverage.unknown_executable_bytes
               << ",\"unproven_padding_bytes\":" << coverage.unproven_padding_bytes
               << ",\"incomplete_initial_required_code_bytes\":"
               << coverage.incomplete_initial_required_code_bytes
               << ",\"uncovered_runtime_materializable_bytes\":"
               << coverage.uncovered_runtime_materializable_bytes
               << ",\"instructions\":" << coverage.instructions
               << ",\"proven_instructions\":" << coverage.proven_instructions
               << ",\"guarded_candidate_instructions\":" << coverage.guarded_candidate_instructions
               << ",\"functions\":" << coverage.functions
               << ",\"resolved_control_flow\":" << coverage.resolved_control_flow
               << ",\"guarded_control_flow\":" << coverage.guarded_control_flow
               << ",\"guarded_complete_control_flow\":" << coverage.guarded_complete_control_flow
               << ",\"guarded_partial_control_flow\":" << coverage.guarded_partial_control_flow
               << ",\"runtime_only_control_flow\":" << coverage.runtime_only_control_flow
               << ",\"unresolved_control_flow\":" << coverage.unresolved_control_flow
               << ",\"unresolved_frontier\":"
               << coverage.guarded_partial_control_flow + coverage.runtime_only_control_flow +
                      coverage.unresolved_control_flow
               << ",\"unknown_instructions\":" << coverage.unknown_instructions
               << ",\"reachable_abort_edges\":" << coverage.reachable_abort_edges
               << ",\"executable_byte_classes\":{";
        for (std::size_t current = 0u; current < coverage.executable_byte_classes.size();
             ++current) {
            if (current != 0u) output << ',';
            output << io::quote_json(analysis::executable_byte_class_name(
                          static_cast<analysis::ExecutableByteClass>(current)))
                   << ':' << coverage.executable_byte_classes[current];
        }
        output << "},\"precompile_sets\":{";
        for (std::size_t current = 0u; current < coverage.precompile_classes.size(); ++current) {
            if (current != 0u) output << ',';
            output << io::quote_json(analysis::precompile_class_name(
                          static_cast<analysis::PrecompileClass>(current)))
                   << ':' << coverage.precompile_classes[current];
        }
        output << "},\"mixed_range_roles\":{";
        for (std::size_t current = 0u; current < coverage.mixed_range_roles.size(); ++current) {
            if (current != 0u) output << ',';
            output << io::quote_json(analysis::mixed_range_role_name(
                          static_cast<analysis::MixedRangeRole>(current)))
                   << ':' << coverage.mixed_range_roles[current];
        }
        output << "},\"range_proof_classes\":{";
        for (std::size_t current = 0u; current < coverage.range_proof_classes.size(); ++current) {
            if (current != 0u) output << ',';
            output << io::quote_json(analysis::range_proof_class_name(
                          static_cast<analysis::RangeProofClass>(current)))
                   << ':' << coverage.range_proof_classes[current];
        }
        output << '}' << ",\"control_flow_complete\":"
               << (coverage.control_flow_complete ? "true" : "false") << '}';
    }
    output << ",\"artifacts\":[";
    for (std::size_t index = 0u; index < result.artifacts.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& artifact = result.artifacts[index];
        output << "{\"role\":" << io::quote_json(artifact.role)
               << ",\"path\":" << io::quote_json(artifact.relative_path.generic_string())
               << ",\"sha256\":" << io::quote_json(artifact.sha256) << ",\"size\":" << artifact.size
               << ",\"format_version\":" << artifact.format_version
               << ",\"job_generation\":" << io::quote_json(artifact.job_generation) << '}';
    }
    output << "],\"diagnostics\":[";
    for (std::size_t index = 0u; index < result.diagnostics.size(); ++index) {
        if (index != 0u) output << ',';
        output << diagnostic_json(result.diagnostics[index]);
    }
    output << "],\"checkpoints\":[";
    for (std::size_t index = 0u; index < result.checkpoints.size(); ++index) {
        if (index != 0u) output << ',';
        output << io::quote_json(result.checkpoints[index]);
    }
    output << "]}\n";
    return output.str();
}

std::string format_job_event_json(const JobEvent& event) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-job-event\",\"version\":1,\"job_id\":"
           << io::quote_json(event.job_id) << ",\"sequence\":" << event.sequence
           << ",\"state\":" << io::quote_json(job_state_name(event.state))
           << ",\"overall_percent\":" << event.progress_percent
           << ",\"stage\":" << io::quote_json(event.stage)
           << ",\"step_status\":" << io::quote_json(job_step_status_name(event.step_status))
           << ",\"step_current\":";
    if (event.step_current)
        output << *event.step_current;
    else
        output << "null";
    output << ",\"step_total\":";
    if (event.step_total)
        output << *event.step_total;
    else
        output << "null";
    output << ",\"timestamp_ms\":" << event.timestamp_ms << ",\"elapsed_ms\":" << event.elapsed_ms
           << ",\"log_chunk\":";
    if (event.log_chunk)
        output << io::quote_json(*event.log_chunk);
    else
        output << "null";
    output << ",\"diagnostic\":";
    if (event.diagnostic)
        output << diagnostic_json(*event.diagnostic);
    else
        output << "null";
    output << "}\n";
    return output.str();
}

} // namespace katana::app
