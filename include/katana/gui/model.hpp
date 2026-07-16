#pragma once

#include "katana/app/application.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace katana::gui {

enum class Page : std::uint8_t { Dashboard, Project, Source, Jobs, Diagnostics, Results, Settings };

struct ShellSnapshot {
    Page page = Page::Dashboard;
    std::string project_name;
    std::string source_name;
    std::string source_format;
    std::size_t source_track_count = 0u;
    bool project_dirty = false;
    bool job_active = false;
    std::uint32_t job_progress = 0u;
    std::string job_stage;
    std::vector<app::Diagnostic> diagnostics;
    std::vector<app::JobArtifact> artifacts;
};

class Model final {
  public:
    explicit Model(std::filesystem::path settings_path);

    void navigate(Page page);
    void navigate_next();
    void navigate_previous();
    [[nodiscard]] Page page() const noexcept;

    void new_project(const std::filesystem::path& manifest_path,
                     std::string project_name,
                     io::ProjectInputFormat format,
                     const std::filesystem::path& source_path);
    void open_project(const std::filesystem::path& manifest_path);
    void save_project();
    [[nodiscard]] io::ProjectManifest& edit_manifest();
    void refresh_source();
    [[nodiscard]] bool has_project() const noexcept;
    [[nodiscard]] bool has_unsaved_changes() const noexcept;

    [[nodiscard]] app::JobResult run_job(app::JobKind kind,
                                         const std::filesystem::path& output_root,
                                         std::string job_id,
                                         std::string tool_version);
    void cancel_job();
    [[nodiscard]] ShellSnapshot snapshot() const;
    [[nodiscard]] std::string accessible_summary() const;
    [[nodiscard]] std::string automation_snapshot_json() const;

    [[nodiscard]] app::UserSettings& settings() noexcept;
    void persist_settings();
    void recover_settings();

  private:
    void set_failure(const std::exception& error, std::string recovery);

    std::filesystem::path settings_path_;
    app::UserSettings settings_;
    Page page_ = Page::Dashboard;
    std::optional<app::ProjectSession> project_;
    app::SourceInspection source_;
    app::JobCoordinator jobs_;
    std::shared_ptr<app::Cancellation> cancellation_;
    mutable std::mutex mutex_;
    bool job_active_ = false;
    std::uint32_t job_progress_ = 0u;
    std::string job_stage_;
    std::vector<app::Diagnostic> diagnostics_;
    std::vector<app::JobArtifact> artifacts_;
};

[[nodiscard]] const char* page_name(Page page) noexcept;

} // namespace katana::gui
