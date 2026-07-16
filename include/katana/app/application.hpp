#pragma once

#include "katana/io/project_manifest.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace katana::app {

inline constexpr std::uint32_t application_contract_version = 1u;
inline constexpr std::uint32_t settings_schema_version = 1u;

enum class DiagnosticSeverity : std::uint8_t { Information, Warning, Error };

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Information;
    std::string code;
    std::string message;
    std::string recovery;
    std::optional<std::size_t> source_line;
};

struct GdiTrackView {
    std::uint32_t number = 0u;
    std::uint32_t lba = 0u;
    std::string role;
    std::uint32_t sector_size = 0u;
    std::uint64_t file_size = 0u;
    std::uint64_t file_offset = 0u;
    std::uint64_t sector_count = 0u;
    std::size_t descriptor_line = 0u;
    std::string file_name;
    std::string sha256;
};

struct SourceInspection {
    std::string format;
    std::string display_name;
    std::uint64_t size = 0u;
    std::string sha256;
    bool read_only = true;
    std::vector<GdiTrackView> tracks;
    std::vector<Diagnostic> diagnostics;
};

class ProjectSession final {
  public:
    static ProjectSession create(std::filesystem::path manifest_path, io::ProjectManifest manifest);
    static ProjectSession open(const std::filesystem::path& manifest_path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const io::ProjectManifest& manifest() const noexcept;
    [[nodiscard]] io::ProjectManifest& edit() noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    void save();

  private:
    ProjectSession(std::filesystem::path path, io::ProjectManifest manifest, bool dirty);
    std::filesystem::path path_;
    io::ProjectManifest manifest_;
    bool dirty_ = false;
    std::uint64_t revision_ = 0u;
};

struct UserSettings {
    std::uint32_t version = settings_schema_version;
    std::string theme = "system";
    std::uint32_t scale_percent = 100u;
    bool restore_last_project = true;
    std::vector<std::filesystem::path> recent_projects;
};

[[nodiscard]] UserSettings load_user_settings(const std::filesystem::path& path);
void save_user_settings(const std::filesystem::path& path, const UserSettings& settings);
void remember_recent_project(UserSettings& settings, const std::filesystem::path& path);

enum class JobKind : std::uint8_t { Validate, Analyze, Codegen, Build, RunPreflight };
enum class JobState : std::uint8_t { Queued, Running, Completed, Failed, Cancelled };

struct JobRequest {
    std::string id;
    JobKind kind = JobKind::Validate;
    std::filesystem::path manifest_path;
    std::filesystem::path output_root;
    std::string tool_version;
};

struct JobEvent {
    std::string job_id;
    JobState state = JobState::Queued;
    std::uint32_t progress_percent = 0u;
    std::string stage;
    std::optional<Diagnostic> diagnostic;
};

struct JobArtifact {
    std::string role;
    std::filesystem::path relative_path;
    std::string sha256;
};

struct JobResult {
    std::string job_id;
    JobKind kind = JobKind::Validate;
    JobState state = JobState::Failed;
    std::string project_identity;
    std::vector<JobArtifact> artifacts;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> checkpoints;
};

class Cancellation final {
  public:
    void request() noexcept;
    [[nodiscard]] bool requested() const noexcept;

  private:
    std::atomic_bool requested_{false};
};

using JobObserver = std::function<void(const JobEvent&)>;

class ApplicationService final {
  public:
    [[nodiscard]] SourceInspection inspect_source(const io::ProjectManifest& manifest) const;
    [[nodiscard]] JobResult execute(const JobRequest& request,
                                    const std::shared_ptr<Cancellation>& cancellation = {},
                                    const JobObserver& observer = {}) const;
};

class JobCoordinator final {
  public:
    explicit JobCoordinator(ApplicationService service = {});
    [[nodiscard]] JobResult execute(const JobRequest& request,
                                    const std::shared_ptr<Cancellation>& cancellation = {},
                                    const JobObserver& observer = {});

  private:
    ApplicationService service_;
    std::mutex mutex_;
    std::vector<std::filesystem::path> active_outputs_;
};

[[nodiscard]] const char* job_kind_name(JobKind kind) noexcept;
[[nodiscard]] const char* job_state_name(JobState state) noexcept;
[[nodiscard]] std::string redact_sensitive_text(std::string_view text);
[[nodiscard]] std::string format_source_inspection_json(const SourceInspection& inspection);
[[nodiscard]] std::string format_job_result_json(const JobResult& result);

} // namespace katana::app
