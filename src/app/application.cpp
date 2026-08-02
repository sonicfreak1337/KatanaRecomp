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
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/gdi.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <random>
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

enum class PublishedJobResultState : std::uint8_t { Successful, Unsuccessful };

bool unsafe_application_path_link(
    const std::filesystem::path& path,
    const std::filesystem::file_status status) noexcept {
    if (std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
#else
    static_cast<void>(path);
    return false;
#endif
}

void require_no_existing_application_path_links(
    const std::filesystem::path& path,
    const std::string_view description) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    auto current = absolute.root_path();
    const auto inspect = [&](const bool is_leaf) {
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(current, status_error);
        if (status_error == std::errc::no_such_file_or_directory ||
            (!status_error &&
             status.type() == std::filesystem::file_type::not_found))
            return false;
        if (status_error || unsafe_application_path_link(current, status) ||
            (!is_leaf && !std::filesystem::is_directory(status)))
            throw std::runtime_error(
                std::string(description) +
                " besitzt einen unsicheren bestehenden Pfadbestandteil.");
        return true;
    };
    if (!current.empty() && !inspect(current == absolute)) return;
    for (const auto& component : absolute.relative_path()) {
        current /= component;
        if (!inspect(current == absolute)) return;
    }
}

std::string application_output_binding(
    const std::filesystem::path& final_root) {
    return io::sha256_bytes(
        "KATANA_APPLICATION_OUTPUT_BINDING 1\n" +
        normalized_output_path(final_root).generic_string() + '\n');
}

PublishedJobResultState
published_job_result_state(
    const std::filesystem::path& root,
    const std::filesystem::path& expected_final_root) {
    constexpr std::uintmax_t max_job_result_bytes = 16u * 1024u * 1024u;
    require_no_existing_application_path_links(
        root, "Vorhandenes Jobergebnis");
    std::error_code status_error;
    const auto root_status = std::filesystem::symlink_status(root, status_error);
    if (status_error || !std::filesystem::is_directory(root_status) ||
        unsafe_application_path_link(root, root_status))
        throw std::runtime_error(
            "Vorhandenes Jobergebnis besitzt keinen sicheren Ausgabeordner.");

    const auto result_path = root / "job-result.json";
    status_error.clear();
    const auto result_status = std::filesystem::symlink_status(result_path, status_error);
    if (status_error || !std::filesystem::is_regular_file(result_status) ||
        unsafe_application_path_link(result_path, result_status))
        throw std::runtime_error(
            "Vorhandenes Jobergebnis besitzt keinen lesbaren terminalen Status.");
    status_error.clear();
    const auto result_size = std::filesystem::file_size(result_path, status_error);
    if (status_error || result_size > max_job_result_bytes)
        throw std::runtime_error(
            "Vorhandenes Jobergebnis ueberschreitet das sichere Lesebudget.");
    std::ifstream input(result_path, std::ios::binary);
    if (!input)
        throw std::runtime_error(
            "Vorhandenes Jobergebnis ist nicht lesbar.");
    std::string result_json(static_cast<std::size_t>(result_size), '\0');
    if (!result_json.empty()) {
        input.read(result_json.data(), static_cast<std::streamsize>(result_json.size()));
        if (input.gcount() != static_cast<std::streamsize>(result_json.size()))
            throw std::runtime_error(
                "Vorhandenes Jobergebnis konnte nicht vollstaendig gelesen werden.");
    }
    char trailing_byte = '\0';
    if (input.get(trailing_byte) || input.bad())
        throw std::runtime_error(
            "Vorhandenes Jobergebnis hat sich waehrend des Lesens veraendert.");
    const auto contract_prefix =
        "{\"schema\":\"katana-application-job\",\"version\":" +
        std::to_string(application_contract_version) +
        ",\"output_binding\":\"" +
        application_output_binding(expected_final_root) + "\",";
    if (!result_json.starts_with(contract_prefix))
        throw std::runtime_error(
            "Vorhandenes Jobergebnis besitzt keinen Katana-Anwendungsvertrag.");
    constexpr std::string_view state_prefix = ",\"state\":\"";
    const auto state_begin = result_json.find(state_prefix);
    if (state_begin == std::string::npos)
        throw std::runtime_error(
            "Vorhandenes Jobergebnis besitzt keinen terminalen Status.");
    const auto value_begin = state_begin + state_prefix.size();
    const auto value_end = result_json.find('"', value_begin);
    if (value_end == std::string::npos)
        throw std::runtime_error(
            "Vorhandenes Jobergebnis besitzt einen ungueltigen Status.");
    const std::string_view state(result_json.data() + value_begin,
                                 value_end - value_begin);
    if (state == "completed" || state == "partial")
        return PublishedJobResultState::Successful;
    if (state == "failed" || state == "cancelled")
        return PublishedJobResultState::Unsuccessful;
    throw std::runtime_error(
        "Vorhandenes Jobergebnis ist nicht terminal.");
}

constexpr std::string_view stale_cleanup_proof_name =
    ".katana-stale-cleanup-proof";

std::string new_application_stage_token(
    const std::filesystem::path& final_root,
    const std::string_view job_id) {
    std::random_device random;
    std::ostringstream seed;
    seed << normalized_output_path(final_root).generic_string() << ':'
         << job_id << ':'
         << std::chrono::steady_clock::now().time_since_epoch().count()
         << ':' << random() << ':' << random() << ':' << random()
         << ':' << random();
#ifdef _WIN32
    seed << ':' << GetCurrentProcessId();
#else
    seed << ':' << ::getpid();
#endif
    return io::sha256_bytes(seed.str()).substr(0u, 24u);
}

std::string stale_cleanup_proof_content(
    const std::filesystem::path& final_root,
    const std::filesystem::path& stale_root) {
    const auto binding = io::sha256_bytes(
        normalized_output_path(final_root).generic_string() + '\n' +
        normalized_output_path(stale_root).generic_string());
    return "KATANA_STALE_CLEANUP_PROOF 1\nbinding " + binding + "\n";
}

bool safe_transaction_directory_exists(
    const std::filesystem::path& root,
    const std::string_view description) {
    require_no_existing_application_path_links(root, description);
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(root, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() == std::filesystem::file_type::not_found))
        return false;
    if (status_error || !std::filesystem::is_directory(status) ||
        unsafe_application_path_link(root, status))
        throw std::runtime_error(
            std::string(description) +
            " ist kein sicherer regulaerer Ordner.");
    return true;
}

void require_safe_transaction_tree(
    const std::filesystem::path& root,
    const std::string_view description) {
    if (!safe_transaction_directory_exists(root, description))
        throw std::runtime_error(
            std::string(description) + " fehlt.");
    for (std::filesystem::recursive_directory_iterator iterator(root), end;
         iterator != end;
         ++iterator) {
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error ||
            unsafe_application_path_link(iterator->path(), status) ||
            (!std::filesystem::is_directory(status) &&
             !std::filesystem::is_regular_file(status)))
            throw std::runtime_error(
                std::string(description) +
                " enthaelt einen unsicheren Dateisystemeintrag.");
    }
}

void create_safe_transaction_directory(
    const std::filesystem::path& root,
    const std::string_view description) {
    if (safe_transaction_directory_exists(root, description))
        throw std::runtime_error(
            std::string(description) + " ist bereits vorhanden.");
    std::error_code create_error;
    if (!std::filesystem::create_directory(root, create_error) || create_error)
        throw std::runtime_error(
            std::string(description) + " konnte nicht erstellt werden.");
    if (!safe_transaction_directory_exists(root, description))
        throw std::runtime_error(
            std::string(description) + " wurde nicht sicher erstellt.");
}

void require_safe_stale_cleanup_root(
    const std::filesystem::path& stale_root) {
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(stale_root, status_error);
    if (status_error || !std::filesystem::is_directory(status) ||
        unsafe_application_path_link(stale_root, status))
        throw std::runtime_error(
            "Recovery-Kopie ist kein sicherer regulaerer Ordner.");
}

bool has_valid_stale_cleanup_proof(
    const std::filesystem::path& final_root,
    const std::filesystem::path& stale_root) {
    constexpr std::uintmax_t maximum_proof_bytes = 256u;
    const auto proof_path = final_root / stale_cleanup_proof_name;
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(proof_path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() == std::filesystem::file_type::not_found))
        return false;
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_application_path_link(proof_path, status))
        throw std::runtime_error(
            "Recovery-Cleanup-Nachweis ist kein sicherer regulaerer Marker.");
    status_error.clear();
    const auto size = std::filesystem::file_size(proof_path, status_error);
    if (status_error || size > maximum_proof_bytes)
        throw std::runtime_error(
            "Recovery-Cleanup-Nachweis ueberschreitet sein Lesebudget.");
    std::ifstream input(proof_path, std::ios::binary);
    if (!input)
        throw std::runtime_error(
            "Recovery-Cleanup-Nachweis ist nicht lesbar.");
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!content.empty()) {
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (input.gcount() != static_cast<std::streamsize>(content.size()))
            throw std::runtime_error(
                "Recovery-Cleanup-Nachweis konnte nicht vollstaendig gelesen werden.");
    }
    char trailing_byte = '\0';
    if (input.get(trailing_byte) || input.bad() ||
        content != stale_cleanup_proof_content(final_root, stale_root))
        throw std::runtime_error(
            "Recovery-Cleanup-Nachweis ist fremd oder ungueltig.");
    return true;
}

void write_stale_cleanup_proof(
    const std::filesystem::path& final_root,
    const std::filesystem::path& stale_root) {
    const auto proof_path = final_root / stale_cleanup_proof_name;
    auto temporary_path = proof_path;
    temporary_path += ".katana-tmp";
    for (const auto& reserved_path : {proof_path, temporary_path}) {
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(reserved_path, status_error);
        const bool missing =
            status_error == std::errc::no_such_file_or_directory ||
            (!status_error &&
             status.type() == std::filesystem::file_type::not_found);
        if (missing) continue;
        if (status_error || !std::filesystem::is_regular_file(status) ||
            unsafe_application_path_link(reserved_path, status))
            throw std::runtime_error(
                "Reservierter Recovery-Cleanup-Marker ist fremd oder unsicher.");
        if (reserved_path == proof_path)
            throw std::runtime_error(
                "Recovery-Cleanup-Nachweis ist unerwartet bereits vorhanden.");
        if (!std::filesystem::remove(reserved_path, status_error) || status_error)
            throw std::runtime_error(
                "Temporarer Recovery-Cleanup-Marker konnte nicht ersetzt werden.");
    }
    write_atomic(
        proof_path,
        stale_cleanup_proof_content(final_root, stale_root));
}

void remove_stale_cleanup_proof(
    const std::filesystem::path& final_root,
    const std::filesystem::path& stale_root) {
    if (!has_valid_stale_cleanup_proof(final_root, stale_root))
        throw std::runtime_error(
            "Recovery-Cleanup-Nachweis fehlt vor seiner Entfernung.");
    std::error_code remove_error;
    if (!std::filesystem::remove(
            final_root / stale_cleanup_proof_name, remove_error) ||
        remove_error)
        throw std::runtime_error(
            "Recovery-Cleanup-Nachweis konnte nicht entfernt werden.");
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

bool remove_tree_with_retry(
    const std::filesystem::path& path,
    std::error_code& error,
    const std::function<void()>& validate_before_attempt = {}) {
    constexpr std::size_t attempts = 100u;
    constexpr auto retry_delay = std::chrono::milliseconds(50);
    auto removal_path = path;
#ifdef _WIN32
    std::error_code absolute_error;
    auto absolute = std::filesystem::absolute(path, absolute_error);
    if (!absolute_error) {
        absolute.make_preferred();
        const auto native = absolute.native();
        if (!native.starts_with(LR"(\\?\)")) {
            auto extended = native.starts_with(LR"(\\)")
                                ? std::wstring(LR"(\\?\UNC\)") + native.substr(2u)
                                : std::wstring(LR"(\\?\)") + native;
            removal_path = std::filesystem::path(std::move(extended));
        } else {
            removal_path = absolute;
        }
    }
#endif
    for (std::size_t attempt = 0u; attempt < attempts; ++attempt) {
        if (validate_before_attempt) validate_before_attempt();
        error.clear();
        std::filesystem::remove_all(removal_path, error);
        if (!error) return true;

        std::error_code exists_error;
        if (!std::filesystem::exists(removal_path, exists_error) && !exists_error) {
            error.clear();
            return true;
        }
        if (attempt + 1u != attempts) std::this_thread::sleep_for(retry_delay);
    }
    return false;
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

std::optional<std::string> configured_environment_value(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t value_size = 0u;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
    if (result.empty()) return std::nullopt;
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
#endif
}

std::size_t configured_host_build_jobs() {
    constexpr std::size_t maximum_jobs = 256u;
    auto configured = configured_environment_value("KATANA_HOST_BUILD_JOBS");
    if (!configured) configured = configured_environment_value("KATANA_PORT_CODEGEN_JOBS");
    if (!configured) {
        return std::min<std::size_t>(maximum_jobs,
                                     std::max(1u, std::thread::hardware_concurrency()));
    }
    std::size_t parsed = 0u;
    const auto jobs = std::stoull(*configured, &parsed, 10);
    if (parsed != configured->size() || jobs == 0u || jobs > maximum_jobs)
        throw std::invalid_argument("KATANA_HOST_BUILD_JOBS ist ungueltig.");
    return static_cast<std::size_t>(jobs);
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
        configure += " -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo";
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
    configure += " -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo";
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
    compile += " --parallel " + std::to_string(configured_host_build_jobs());
#ifdef _WIN32
    if (!use_ninja) compile += " --config RelWithDebInfo -- /nodeReuse:false";
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
    coverage.unknown_instructions =
        static_cast<std::size_t>(std::count_if(analysis.recursive.diagnostics.begin(),
                                               analysis.recursive.diagnostics.end(),
                                               analysis::analysis_diagnostic_blocks_codegen));
    coverage.candidate_unknown_instructions =
        analysis.recursive.diagnostics.size() - coverage.unknown_instructions;
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
    std::unordered_set<std::uint32_t> statically_required_instruction_addresses;
    for (const auto& contextual : analysis.recursive.contextual_instructions) {
        if (analysis::control_flow_evidence_requires_static_decode(contextual.evidence))
            statically_required_instruction_addresses.insert(contextual.line.address);
    }
    const auto statically_required_instruction = [&](const std::uint32_t address) {
        return statically_required_instruction_addresses.contains(address);
    };
    std::set<std::pair<std::uint32_t, std::uint32_t>> invalid_edges;
    const auto add_invalid_edge = [&](const std::uint32_t source, const std::uint32_t target) {
        if (!statically_required_instruction(source)) return;
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
        if (statically_required_instruction(line.address) && line.target_address.has_value())
            required_targets.insert(*line.target_address);
    for (const auto& edge : analysis.resolved_edges)
        if (analysis::control_flow_evidence_requires_static_decode(
                analysis::resolved_edge_evidence(edge)))
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
           << ",\"candidate_unknown_instructions\":" << coverage.candidate_unknown_instructions
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
    const auto final_root =
        std::filesystem::absolute(request.output_root).lexically_normal();
    require_no_existing_application_path_links(
        final_root, "Angefordertes Ausgabeziel");
    CrossProcessJobLock process_lock(normalized_output_path(final_root));
    JobResult result;
    result.job_id = request.id;
    result.output_binding = application_output_binding(final_root);
    result.kind = request.kind;
    result.state = JobState::Running;
    result.tool_version = request.tool_version;
    result.failure_category = JobFailureCategory::InputOutput;
    const bool transactional =
        request.kind == JobKind::Build || request.kind == JobKind::RunPreflight;
    auto work_root = final_root;
    auto stale_root = final_root;
    stale_root += ".katana-stale";
    bool transactional_setup_complete = !transactional;
    bool previous_final_rotated = false;
    bool published_success = false;
    bool work_root_owned = false;
    if (transactional) {
        work_root =
            final_root.parent_path() /
            (".katana-stage-" +
             new_application_stage_token(final_root, request.id));
    }
    JobEventStream events(request, observer);
    events.emit(JobState::Queued, 0u, "queued", {}, JobStepStatus::Pending);
    events.emit(JobState::Running, 2u, "validation", {}, JobStepStatus::Running);
    try {
        if (transactional) {
            std::error_code cleanup_error;
            auto final_exists = safe_transaction_directory_exists(
                final_root, "Vorhandenes Ausgabeziel");
            if (final_exists) {
                std::error_code empty_error;
                const auto final_status =
                    std::filesystem::symlink_status(final_root, empty_error);
                if (empty_error ||
                    unsafe_application_path_link(final_root, final_status))
                    throw std::runtime_error(
                        "Vorhandenes Ausgabeziel konnte nicht sicher geprueft werden.");
                if (std::filesystem::is_directory(final_status) &&
                    std::filesystem::is_empty(final_root, empty_error)) {
                    if (empty_error ||
                        !safe_transaction_directory_exists(
                            final_root, "Leeres Ausgabeziel") ||
                        !std::filesystem::is_empty(final_root, empty_error) ||
                        empty_error ||
                        !std::filesystem::remove(final_root, empty_error) ||
                        empty_error)
                        throw std::runtime_error(
                            "Leeres Ausgabeziel konnte nicht fuer den ersten Lauf vorbereitet "
                            "werden.");
                    final_exists = false;
                } else if (empty_error) {
                    throw std::runtime_error(
                        "Vorhandenes Ausgabeziel konnte nicht sicher geprueft werden.");
                }
            }
            const auto stale_exists = safe_transaction_directory_exists(
                stale_root, "Vorhandene Recovery-Kopie");
            std::optional<PublishedJobResultState> final_state;
            bool stale_cleanup_proven = false;
            if (final_exists) {
                final_state = published_job_result_state(
                    final_root, final_root);
                stale_cleanup_proven = has_valid_stale_cleanup_proof(
                    final_root, stale_root);
                if (stale_cleanup_proven &&
                    *final_state != PublishedJobResultState::Successful)
                    throw std::runtime_error(
                        "Fehlerhaftes Jobergebnis besitzt einen fremden "
                        "Recovery-Cleanup-Nachweis.");
            }
            if (stale_exists) {
                require_safe_stale_cleanup_root(stale_root);
                if (!stale_cleanup_proven)
                    static_cast<void>(
                        published_job_result_state(stale_root, final_root));
            }
            if (final_exists && stale_exists) {
                if (*final_state == PublishedJobResultState::Successful) {
                    if (!stale_cleanup_proven) {
                        codegen::preserve_local_port_user_data(
                            stale_root, final_root);
                        write_stale_cleanup_proof(
                            final_root, stale_root);
                        stale_cleanup_proven = true;
                    }
                    if (!remove_tree_with_retry(
                            stale_root,
                            cleanup_error,
                            [&] {
                                if (!has_valid_stale_cleanup_proof(
                                    final_root,
                                    stale_root))
                                    throw std::runtime_error(
                                        "Recovery-Cleanup-Nachweis fehlt.");
                                require_safe_transaction_tree(
                                    stale_root,
                                    "Zu bereinigende Recovery-Kopie");
                            }))
                        throw std::runtime_error(
                            "Unterbrochene Erfolgsveroeffentlichung konnte nicht bereinigt werden.");
                    remove_stale_cleanup_proof(
                        final_root, stale_root);
                    stale_cleanup_proven = false;
                } else {
                    require_safe_transaction_tree(
                        final_root,
                        "Zu ersetzender Fehlerbericht");
                    if (safe_transaction_directory_exists(
                            work_root, "Fehlerbericht-Quarantaene"))
                        throw std::runtime_error(
                            "Fehlerbericht-Quarantaene ist unerwartet belegt.");
                    std::filesystem::rename(final_root, work_root);
                    work_root_owned = true;
                    if (!remove_tree_with_retry(
                            work_root,
                            cleanup_error,
                            [&] {
                                if (!work_root_owned)
                                    throw std::runtime_error(
                                        "Job-Staging ist nicht transaktionseigen.");
                                require_safe_transaction_tree(
                                    work_root,
                                    "Transaktionseigener Fehlerbericht");
                            }))
                        throw std::runtime_error(
                            "Vorheriger Fehlerbericht konnte nicht ersetzt werden.");
                    work_root_owned = false;
                }
            } else if (final_exists && stale_cleanup_proven) {
                remove_stale_cleanup_proof(
                    final_root, stale_root);
            }
            if (safe_transaction_directory_exists(
                    final_root, "Zu sicherndes Jobergebnis")) {
                if (safe_transaction_directory_exists(
                        stale_root, "Recovery-Ziel"))
                    throw std::runtime_error(
                        "Recovery-Ziel ist vor der Rotation nicht frei.");
                std::filesystem::rename(final_root, stale_root);
                previous_final_rotated = true;
            }
            create_safe_transaction_directory(
                work_root, "Neues Job-Staging");
            work_root_owned = true;
            transactional_setup_complete = true;
        }
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
                        const auto boot_segment =
                            std::find_if(project.image.segments().begin(),
                                         project.image.segments().end(),
                                         [](const auto& segment) {
                                             return segment.virtual_address ==
                                                    platform::dreamcast_disc_boot_address;
                                         });
                        const auto boot_size = boot_segment == project.image.segments().end()
                                                   ? 0u
                                                   : boot_segment->bytes.size();
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
                                          "RelWithDebInfo" / "game.exe";
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
                        const auto published_recipe = content_root / "game.katana-install";
                        std::filesystem::copy_file(
                            port_export.disc_install_recipe,
                            published_recipe,
                            std::filesystem::copy_options::overwrite_existing);
                        const auto recipe = runtime::parse_disc_install_recipe(published_recipe);
                        if (recipe.job_generation != result.project_identity ||
                            recipe.content_identity != port_export.content_identity)
                            throw std::runtime_error(
                                "Disc-Installations-Recipe besitzt eine falsche Jobbindung.");
                        const auto recipe_sha256 = artifact_hash(published_recipe);
                        const auto published_manifest = content_root / "game.katana-install.json";
                        write_atomic(published_manifest,
                                     "{\"schema\":\"katana-disc-install\",\"version\":1,"
                                     "\"job_generation\":\"" +
                                         result.project_identity + "\",\"content_identity\":\"" +
                                         recipe.content_identity +
                                         "\",\"artifacts\":[{\"role\":"
                                         "\"disc_install_recipe\",\"path\":"
                                         "\"game.katana-install\",\"sha256\":\"" +
                                         recipe_sha256 +
                                         "\"},{\"role\":\"host_executable\",\"path\":"
                                         "\"../" +
                                         executable.filename().generic_string() +
                                         "\",\"sha256\":\"" + executable_sha256 + "\"}]}\n");
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
                            {"disc_install_recipe",
                             std::filesystem::path("content") / "game.katana-install",
                             recipe_sha256,
                             std::filesystem::file_size(published_recipe),
                             runtime::disc_install_recipe_version,
                             result.project_identity});
                        result.artifacts.push_back(
                            {"disc_install_manifest",
                             std::filesystem::path("content") / "game.katana-install.json",
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
                        if (!remove_tree_with_retry(host_build_root, cleanup_error))
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
    bool current_result_published = false;
    bool failure_final_owned = false;
    const auto final_directory_exists = [&] {
        return safe_transaction_directory_exists(
            final_root, "Transaktionales Jobergebnis");
    };
    const auto stale_directory_exists = [&] {
        return safe_transaction_directory_exists(
            stale_root, "Transaktionale Recovery-Kopie");
    };
    const auto remove_owned_work = [&](std::error_code& cleanup_error) {
        if (!work_root_owned) {
            if (safe_transaction_directory_exists(
                    work_root, "Nicht eigenes Job-Staging"))
                throw std::runtime_error(
                    "Nicht eigenes Job-Staging bleibt unangetastet.");
            return true;
        }
        const auto removed = remove_tree_with_retry(
            work_root,
            cleanup_error,
            [&] {
                if (!work_root_owned)
                    throw std::runtime_error(
                        "Job-Staging ist nicht transaktionseigen.");
                if (safe_transaction_directory_exists(
                        work_root, "Transaktionseigenes Job-Staging"))
                    require_safe_transaction_tree(
                        work_root, "Transaktionseigenes Job-Staging");
            });
        if (removed) work_root_owned = false;
        return removed;
    };
    const auto publish_failure_final = [&] {
        if (final_directory_exists())
            throw std::runtime_error(
                "Fehlerbericht kann kein bestehendes Jobergebnis ersetzen.");
        std::error_code cleanup_error;
        if (!remove_owned_work(cleanup_error))
            throw std::runtime_error(
                "Altes Job-Staging konnte vor dem Fehlerbericht nicht bereinigt werden.");
        create_safe_transaction_directory(
            work_root, "Fehlerbericht-Staging");
        work_root_owned = true;
        result_path = work_root / "job-result.json";
        write_atomic(result_path, format_job_result_json(result));
        std::filesystem::rename(work_root, final_root);
        work_root_owned = false;
        if (!final_directory_exists())
            throw std::runtime_error(
                "Fehlerbericht wurde nicht sicher publiziert.");
        failure_final_owned = true;
        result_path = final_root / "job-result.json";
        current_result_published = true;
    };
    const auto remove_owned_failure_final = [&](std::error_code& cleanup_error) {
        if (!failure_final_owned) return true;
        if (!final_directory_exists()) {
            failure_final_owned = false;
            return true;
        }
        if (!remove_owned_work(cleanup_error)) return false;
        require_safe_transaction_tree(
            final_root, "Transaktionseigener Fehlerbericht");
        std::filesystem::rename(final_root, work_root);
        failure_final_owned = false;
        work_root_owned = true;
        return remove_owned_work(cleanup_error);
    };
    try {
        if (transactional && result.state != JobState::Completed &&
            result.state != JobState::Partial)
            result.artifacts.clear();
        if (transactional && !transactional_setup_complete) {
            auto final_exists = final_directory_exists();
            const auto stale_exists = stale_directory_exists();
            std::optional<PublishedJobResultState> stale_state;
            if (stale_exists)
                stale_state = published_job_result_state(
                    stale_root, final_root);
            if (previous_final_rotated && !final_exists && stale_exists) {
                std::filesystem::rename(stale_root, final_root);
                previous_final_rotated = false;
                final_exists = true;
            }
            if (!final_exists && stale_exists &&
                *stale_state ==
                    PublishedJobResultState::Successful) {
                std::filesystem::rename(stale_root, final_root);
                final_exists = true;
            }
            if (!final_exists) publish_failure_final();
        } else if (!transactional) {
            write_atomic(result_path, format_job_result_json(result));
        } else if (result.state == JobState::Completed ||
                   result.state == JobState::Partial) {
            if (!work_root_owned)
                throw std::runtime_error(
                    "Erfolgreiches Job-Staging ist nicht transaktionseigen.");
            require_safe_transaction_tree(
                work_root, "Erfolgreiches Job-Staging");
            if (final_directory_exists())
                throw std::runtime_error(
                    "Erfolgreiches Job-Staging kann kein bestehendes Ziel ersetzen.");
            write_atomic(result_path, format_job_result_json(result));
            std::filesystem::rename(work_root, final_root);
            work_root_owned = false;
            const auto stale_exists = stale_directory_exists();
            if (stale_exists)
                static_cast<void>(
                    published_job_result_state(stale_root, final_root));
            try {
                if (stale_exists)
                    codegen::preserve_local_port_user_data(
                        stale_root, final_root);
            } catch (...) {
                std::error_code rollback_error;
                if (safe_transaction_directory_exists(
                        work_root, "Rollback-Staging"))
                    throw std::runtime_error(
                        "Rollback-Staging ist unerwartet belegt.");
                require_safe_transaction_tree(
                    final_root, "Noch nicht committedes Jobergebnis");
                std::filesystem::rename(final_root, work_root);
                work_root_owned = true;
                if (stale_directory_exists()) {
                    static_cast<void>(
                        published_job_result_state(stale_root, final_root));
                    std::filesystem::rename(stale_root, final_root);
                    previous_final_rotated = false;
                }
                if (!remove_owned_work(rollback_error))
                    throw std::runtime_error(
                        "Neues Jobergebnis konnte nach dem Rollback nicht bereinigt werden.");
                throw;
            }
            // Preserve kann user-data atomar aus stale in final verschoben
            // haben. Ab hier bleibt das neue erfolgreiche final autoritativ;
            // auch ein nachfolgender Proof-/Cleanupfehler darf es nicht mehr
            // zurueckrollen oder als Fehlerbericht ueberschreiben.
            published_success = true;
            if (stale_exists) {
                write_stale_cleanup_proof(
                    final_root, stale_root);
                std::error_code cleanup_error;
                if (!remove_tree_with_retry(
                        stale_root,
                        cleanup_error,
                        [&] {
                            if (!has_valid_stale_cleanup_proof(
                                final_root,
                                stale_root))
                                throw std::runtime_error(
                                    "Recovery-Cleanup-Nachweis fehlt.");
                            require_safe_transaction_tree(
                                stale_root,
                                "Zu bereinigende Recovery-Kopie");
                        })) {
                    result.diagnostics.push_back(
                        {DiagnosticSeverity::Warning,
                         "job-stale-cleanup-deferred",
                         "Das erfolgreiche Jobergebnis wurde veroeffentlicht, aber die alte "
                         "Recovery-Kopie konnte noch nicht entfernt werden.",
                         "Der naechste Lauf setzt die Bereinigung unter demselben Ausgabe-Lock "
                         "fort.",
                         std::nullopt});
                } else {
                    remove_stale_cleanup_proof(
                        final_root, stale_root);
                    previous_final_rotated = false;
                }
            } else {
                previous_final_rotated = false;
            }
            current_result_published = true;
            result_path = final_root / "job-result.json";
        } else {
            std::error_code cleanup_error;
            if (!remove_owned_work(cleanup_error))
                throw std::runtime_error(
                    "Fehlgeschlagenes Job-Staging konnte nicht bereinigt werden.");
            publish_failure_final();
        }
        if (current_result_published || !transactional)
            result.artifacts.push_back(
                {"job-result", "job-result.json", artifact_hash(result_path)});
    } catch (const std::exception& error) {
        if (published_success) {
            result.diagnostics.push_back(
                {DiagnosticSeverity::Warning,
                 "job-publication-cleanup-deferred",
                 redact_sensitive_text(error.what()),
                 "Das erfolgreiche Jobergebnis bleibt autoritativ; die Bereinigung wird beim "
                 "naechsten Lauf erneut versucht.",
                 std::nullopt});
        } else {
            result.state = JobState::Failed;
            result.failure_category = JobFailureCategory::InputOutput;
            result.artifacts.clear();
            result.diagnostics.push_back(
                make_error("job-publication-failed",
                           error.what(),
                           "Ausgabeziel, Zugriffsrechte und freien Speicher pruefen."));
            if (transactional) {
                std::error_code ignored;
                try {
                    if (!remove_owned_work(ignored))
                        throw std::runtime_error(
                            "Unvollstaendiges Job-Staging konnte nicht entfernt werden.");
                    if (!remove_owned_failure_final(ignored))
                        throw std::runtime_error(
                            "Unvollstaendiger Fehlerbericht konnte nicht entfernt werden.");
                    auto final_exists = final_directory_exists();
                    const auto stale_exists = stale_directory_exists();
                    std::optional<PublishedJobResultState> stale_state;
                    if (stale_exists)
                        stale_state = published_job_result_state(
                            stale_root, final_root);
                    if (!final_exists && stale_exists &&
                        (previous_final_rotated ||
                         *stale_state ==
                             PublishedJobResultState::Successful)) {
                        std::filesystem::rename(stale_root, final_root);
                        previous_final_rotated = false;
                        final_exists = true;
                    }
                    if (!final_exists) {
                        publish_failure_final();
                        result.artifacts.push_back(
                            {"job-result", "job-result.json", artifact_hash(result_path)});
                    } else if (published_job_result_state(
                                   final_root, final_root) ==
                               PublishedJobResultState::Unsuccessful) {
                        result_path = final_root / "job-result.json";
                        write_atomic(
                            result_path, format_job_result_json(result));
                        result.artifacts.push_back(
                            {"job-result", "job-result.json", artifact_hash(result_path)});
                    }
                } catch (...) {
                    if (failure_final_owned) {
                        std::error_code cleanup_error;
                        try {
                            static_cast<void>(
                                remove_owned_failure_final(cleanup_error));
                        } catch (...) {
                        }
                    }
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
           << ",\"output_binding\":" << io::quote_json(result.output_binding)
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
               << ",\"candidate_unknown_instructions\":" << coverage.candidate_unknown_instructions
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
