#include "katana/gui/model.hpp"

#include "katana/io/json_report.hpp"
#include "katana/platform/dreamcast_disc.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>

namespace katana::gui {
namespace {

constexpr std::array pages{Page::Dashboard,
                           Page::Project,
                           Page::Source,
                           Page::Jobs,
                           Page::Diagnostics,
                           Page::Results,
                           Page::Settings};

std::size_t page_index(const Page page) {
    const auto found = std::find(pages.begin(), pages.end(), page);
    return found == pages.end() ? 0u : static_cast<std::size_t>(found - pages.begin());
}

} // namespace

Model::Model(std::filesystem::path settings_path) : settings_path_(std::move(settings_path)) {
    recover_settings();
}

void Model::navigate(const Page page) {
    std::scoped_lock lock(mutex_);
    page_ = page;
}
void Model::navigate_next() {
    std::scoped_lock lock(mutex_);
    page_ = pages[(page_index(page_) + 1u) % pages.size()];
}
void Model::navigate_previous() {
    std::scoped_lock lock(mutex_);
    page_ = pages[(page_index(page_) + pages.size() - 1u) % pages.size()];
}
Page Model::page() const {
    std::scoped_lock lock(mutex_);
    return page_;
}

void Model::new_project(const std::filesystem::path& manifest_path,
                        std::string project_name,
                        const io::ProjectInputFormat format,
                        const std::filesystem::path& source_path,
                        const bool inspect_source) {
    io::ProjectManifest manifest;
    manifest.project_name = std::move(project_name);
    manifest.format = format;
    manifest.input_path = std::filesystem::absolute(source_path).lexically_normal();
    if (format == io::ProjectInputFormat::RawBinary) {
        manifest.base_address = 0x8C010000u;
        manifest.entry_point = 0x8C010000u;
        manifest.segment_name = ".text";
    } else if (format == io::ProjectInputFormat::DreamcastGdi) {
        manifest.entry_point = platform::dreamcast_disc_boot_address;
    }
    auto project = app::ProjectSession::create(manifest_path, std::move(manifest));
    auto source = inspect_source ? app::ApplicationService{}.inspect_source(project.manifest())
                                 : app::SourceInspection{};
    std::scoped_lock lock(mutex_);
    if (job_active_) throw std::logic_error("Projektwechsel ist waehrend eines Jobs gesperrt.");
    project_ = std::move(project);
    source_ = std::move(source);
    diagnostics_.clear();
    artifacts_.clear();
    page_ = Page::Project;
}

void Model::open_project(const std::filesystem::path& manifest_path) {
    try {
        auto project = app::ProjectSession::open(manifest_path);
        auto source = app::ApplicationService{}.inspect_source(project.manifest());
        std::scoped_lock lock(mutex_);
        if (job_active_) throw std::logic_error("Projektwechsel ist waehrend eines Jobs gesperrt.");
        project_ = std::move(project);
        source_ = std::move(source);
        app::remember_recent_project(settings_, manifest_path);
        app::save_user_settings(settings_path_, settings_);
        diagnostics_.clear();
        artifacts_.clear();
        page_ = Page::Project;
    } catch (const std::exception& error) {
        set_failure(error, "Projektressourcen pruefen oder ein neues Projekt anlegen.");
        throw;
    }
}

void Model::save_project() {
    std::scoped_lock lock(mutex_);
    if (job_active_) throw std::logic_error("Projektspeichern ist waehrend eines Jobs gesperrt.");
    if (!project_) throw std::logic_error("Kein Projekt zum Speichern geoeffnet.");
    project_->save();
    app::remember_recent_project(settings_, project_->path());
    app::save_user_settings(settings_path_, settings_);
}

void Model::update_manifest(const std::function<void(io::ProjectManifest&)>& update) {
    if (!update) throw std::invalid_argument("Manifestaktualisierung fehlt.");
    std::scoped_lock lock(mutex_);
    if (job_active_)
        throw std::logic_error("Manifestbearbeitung ist waehrend eines Jobs gesperrt.");
    if (!project_) throw std::logic_error("Kein Projekt zur Bearbeitung geoeffnet.");
    update(project_->edit());
}

void Model::refresh_source() {
    std::scoped_lock lock(mutex_);
    if (job_active_)
        throw std::logic_error("Quellaktualisierung ist waehrend eines Jobs gesperrt.");
    if (!project_) throw std::logic_error("Quellpruefung braucht ein geoeffnetes Projekt.");
    source_ = app::ApplicationService{}.inspect_source(project_->manifest());
}

bool Model::has_project() const {
    std::scoped_lock lock(mutex_);
    return project_.has_value();
}
bool Model::has_unsaved_changes() const {
    std::scoped_lock lock(mutex_);
    return project_ && project_->dirty();
}

app::JobResult Model::run_job(const app::JobKind kind,
                              const std::filesystem::path& output_root,
                              std::string job_id,
                              std::string tool_version,
                              std::shared_ptr<app::Cancellation> cancellation) {
    if (!cancellation) cancellation = std::make_shared<app::Cancellation>();
    std::filesystem::path manifest_path;
    {
        std::scoped_lock lock(mutex_);
        if (job_active_) throw std::logic_error("Ein GUI-Job ist bereits aktiv.");
        if (!project_) throw std::logic_error("Jobstart braucht ein geoeffnetes Projekt.");
        if (project_->dirty()) {
            project_->save();
            app::remember_recent_project(settings_, project_->path());
            app::save_user_settings(settings_path_, settings_);
        }
        manifest_path = project_->path();
        job_active_ = true;
        job_progress_ = 0u;
        job_stage_ = "queued";
        cancellation_ = cancellation;
    }
    try {
        app::JobRequest request{};
        request.id = std::move(job_id);
        request.kind = kind;
        request.manifest_path = manifest_path;
        request.output_root = output_root;
        request.tool_version = std::move(tool_version);
        auto result = jobs_.execute(request, cancellation, [this](const app::JobEvent& event) {
            std::scoped_lock lock(mutex_);
            job_progress_ = event.progress_percent;
            job_stage_ = event.stage;
            if (event.diagnostic) diagnostics_.push_back(*event.diagnostic);
        });
        {
            std::scoped_lock lock(mutex_);
            job_active_ = false;
            diagnostics_ = result.diagnostics;
            artifacts_ = result.artifacts;
            page_ = result.state == app::JobState::Completed ? Page::Results : Page::Diagnostics;
        }
        return result;
    } catch (const std::exception& error) {
        {
            std::scoped_lock lock(mutex_);
            job_active_ = false;
        }
        set_failure(error, "Jobparameter pruefen und den Job erneut starten.");
        throw;
    }
}

void Model::cancel_job() {
    std::scoped_lock lock(mutex_);
    if (cancellation_) cancellation_->request();
}

ShellSnapshot Model::snapshot() const {
    std::scoped_lock lock(mutex_);
    ShellSnapshot result;
    result.page = page_;
    if (project_) {
        result.project_name = project_->manifest().project_name;
        result.project_dirty = project_->dirty();
    }
    result.source_name = source_.display_name;
    result.source_format = source_.format;
    result.source_track_count = source_.tracks.size();
    result.job_active = job_active_;
    result.job_progress = job_progress_;
    result.job_stage = job_stage_;
    result.diagnostics = diagnostics_;
    result.artifacts = artifacts_;
    return result;
}

std::string Model::accessible_summary() const {
    const auto state = snapshot();
    std::ostringstream output;
    output << "KatanaRecomp. Ansicht " << page_name(state.page) << ". ";
    if (state.project_name.empty())
        output << "Kein Projekt geoeffnet. ";
    else
        output << "Projekt " << state.project_name << ". Quelle " << state.source_name
               << ", Format " << state.source_format << ", " << state.source_track_count
               << " Tracks. ";
    if (state.job_active)
        output << "Job " << state.job_stage << ", " << state.job_progress << " Prozent. ";
    output << state.diagnostics.size() << " Diagnosen und " << state.artifacts.size()
           << " Ergebnisartefakte.";
    return output.str();
}

std::string Model::automation_snapshot_json() const {
    const auto state = snapshot();
    std::ostringstream output;
    output << "{\"schema\":\"katana-gui-shell\",\"version\":1,\"page\":"
           << io::quote_json(page_name(state.page))
           << ",\"project\":" << io::quote_json(state.project_name)
           << ",\"source\":" << io::quote_json(state.source_name)
           << ",\"source_format\":" << io::quote_json(state.source_format)
           << ",\"dirty\":" << (state.project_dirty ? "true" : "false")
           << ",\"job_active\":" << (state.job_active ? "true" : "false")
           << ",\"job_progress\":" << state.job_progress
           << ",\"diagnostic_count\":" << state.diagnostics.size()
           << ",\"artifact_count\":" << state.artifacts.size() << "}\n";
    return output.str();
}

app::UserSettings Model::settings() const {
    std::scoped_lock lock(mutex_);
    return settings_;
}
void Model::update_settings(const std::function<void(app::UserSettings&)>& update) {
    if (!update) throw std::invalid_argument("Einstellungsaktualisierung fehlt.");
    std::scoped_lock lock(mutex_);
    update(settings_);
}
void Model::persist_settings() {
    std::scoped_lock lock(mutex_);
    app::save_user_settings(settings_path_, settings_);
}

void Model::recover_settings() {
    try {
        auto settings = app::load_user_settings(settings_path_);
        std::scoped_lock lock(mutex_);
        settings_ = std::move(settings);
    } catch (const std::exception& error) {
        std::scoped_lock lock(mutex_);
        settings_ = {};
        diagnostics_.push_back({app::DiagnosticSeverity::Error,
                                "gui-recovery",
                                app::redact_sensitive_text(error.what()),
                                "Standardeinstellungen wurden wiederhergestellt.",
                                std::nullopt});
        page_ = Page::Diagnostics;
    }
}

void Model::set_failure(const std::exception& error, std::string recovery) {
    std::scoped_lock lock(mutex_);
    diagnostics_.push_back({app::DiagnosticSeverity::Error,
                            "gui-recovery",
                            app::redact_sensitive_text(error.what()),
                            std::move(recovery),
                            std::nullopt});
    page_ = Page::Diagnostics;
}

const char* page_name(const Page page) noexcept {
    switch (page) {
    case Page::Dashboard:
        return "dashboard";
    case Page::Project:
        return "project";
    case Page::Source:
        return "source";
    case Page::Jobs:
        return "jobs";
    case Page::Diagnostics:
        return "diagnostics";
    case Page::Results:
        return "results";
    case Page::Settings:
        return "settings";
    }
    return "unknown";
}

} // namespace katana::gui
