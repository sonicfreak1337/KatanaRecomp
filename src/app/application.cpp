#include "katana/app/application.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/project.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/runtime/gdi.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace katana::app {
namespace {

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
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Ausgabedatei konnte nicht atomar ersetzt werden.");
    }
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

void notify(const JobObserver& observer,
            const JobRequest& request,
            const JobState state,
            const std::uint32_t progress,
            std::string stage,
            std::optional<Diagnostic> diagnostic = std::nullopt) {
    if (observer) observer({request.id, state, progress, std::move(stage), std::move(diagnostic)});
}

void require_not_cancelled(const std::shared_ptr<Cancellation>& cancellation) {
    if (cancellation && cancellation->requested()) throw JobState::Cancelled;
}

std::string project_identity(const io::ProjectManifest& manifest,
                             const std::filesystem::path& manifest_path,
                             const SourceInspection& source) {
    const auto portable_manifest = io::serialize_project_manifest(manifest, manifest_path);
    std::ostringstream identity;
    identity << portable_manifest << "\nsource=" << source.sha256 << ':' << source.size;
    for (const auto& track : source.tracks) {
        identity << "\ntrack=" << track.number << ':' << track.file_size << ':' << track.sector_size
                 << ':' << track.sha256;
    }
    return io::sha256_bytes(identity.str());
}

std::string hex_address(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
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

} // namespace

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

SourceInspection ApplicationService::inspect_source(const io::ProjectManifest& manifest) const {
    SourceInspection result;
    result.format = io::project_input_format_name(manifest.format);
    result.display_name = portable_name(manifest.input_path);
    if (manifest.format != io::ProjectInputFormat::DreamcastGdi) {
        const auto provenance = io::capture_input_provenance("project-input", manifest.input_path);
        result.size = provenance.size;
        result.sha256 = provenance.sha256;
        return result;
    }
    const auto descriptor = runtime::parse_gdi_descriptor(manifest.input_path);
    const auto descriptor_provenance =
        io::capture_input_provenance("gdi-descriptor", manifest.input_path);
    result.size = descriptor_provenance.size;
    result.sha256 = descriptor_provenance.sha256;
    for (const auto& track : descriptor.tracks) {
        const auto provenance = io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path);
        result.tracks.push_back({track.number,
                                 track.lba,
                                 track.type == runtime::GdiTrackType::Data ? "data" : "audio",
                                 track.sector_size,
                                 std::filesystem::file_size(track.resolved_path),
                                 track.file_offset,
                                 track.sector_count,
                                 track.descriptor_line,
                                 portable_name(track.resolved_path),
                                 provenance.sha256});
    }
    return result;
}

JobResult ApplicationService::execute(const JobRequest& request,
                                      const std::shared_ptr<Cancellation>& cancellation,
                                      const JobObserver& observer) const {
    require_stable_id(request.id, "Job-ID");
    require_stable_id(request.tool_version, "Werkzeugversion");
    if (request.manifest_path.empty() || request.output_root.empty())
        throw std::invalid_argument("Job braucht Projektmanifest und Ausgabeziel.");
    JobResult result{request.id, request.kind, JobState::Running};
    notify(observer, request, JobState::Queued, 0u, "queued");
    notify(observer, request, JobState::Running, 5u, "load-project");
    try {
        require_not_cancelled(cancellation);
        const auto project = io::load_project(request.manifest_path);
        const auto inspection = inspect_source(project.execution_profile);
        result.project_identity =
            project_identity(project.execution_profile, request.manifest_path, inspection);
        result.checkpoints.push_back("project-validated");
        notify(observer, request, JobState::Running, 20u, "source-validated");
        require_not_cancelled(cancellation);
        if (request.kind == JobKind::Validate) {
            const auto report_path = request.output_root / "source-inspection.json";
            write_atomic(report_path, format_source_inspection_json(inspection));
            result.artifacts.push_back(
                {"source-inspection", "source-inspection.json", artifact_hash(report_path)});
        } else {
            std::optional<analysis::AnalysisOverrides> overrides;
            if (project.execution_profile.analysis_overrides_path) {
                overrides = analysis::parse_analysis_overrides(
                    *project.execution_profile.analysis_overrides_path);
            }
            const auto analysis =
                analysis::analyze_control_flow(project.image, overrides ? &*overrides : nullptr);
            result.checkpoints.push_back("analysis-complete");
            notify(observer, request, JobState::Running, 45u, "analysis-complete");
            require_not_cancelled(cancellation);
            const auto analysis_json = analysis::format_control_flow_analysis_json(analysis);
            const auto analysis_path = request.output_root / "analysis.json";
            write_atomic(analysis_path, analysis_json);
            result.artifacts.push_back({"analysis", "analysis.json", artifact_hash(analysis_path)});
            const auto result_index_path = request.output_root / "result-index.json";
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
            if (request.kind != JobKind::Analyze) {
                auto program = ir::lower_program(analysis);
                static_cast<void>(ir::optimize_program(program));
                ir::require_valid_program(program);
                const auto entry = project.execution_profile.entry_point.value_or(
                    project.image.entry_points().empty() ? 0u
                                                         : project.image.entry_points().front());
                if (entry == 0u)
                    throw std::runtime_error("Projekt besitzt keinen Codegen-Einstieg.");
                const auto source = codegen::emit_cpp_program(program, entry);
                result.checkpoints.push_back("codegen-complete");
                notify(observer, request, JobState::Running, 70u, "codegen-complete");
                require_not_cancelled(cancellation);
                const auto write = codegen::write_codegen_project(request.output_root / "generated",
                                                                  {{"program.cpp", source}});
                for (const auto& relative : write.written_files) {
                    const auto path = request.output_root / "generated" / relative;
                    result.artifacts.push_back({"generated",
                                                std::filesystem::path("generated") / relative,
                                                artifact_hash(path)});
                }
                if (request.kind == JobKind::Build || request.kind == JobKind::RunPreflight) {
                    result.checkpoints.push_back("build-project-ready");
                    const auto build_report = request.output_root / "build-plan.json";
                    write_atomic(build_report,
                                 "{\"schema\":\"katana-build-plan\",\"version\":1,"
                                 "\"status\":\"ready\",\"native_execution\":false}\n");
                    result.artifacts.push_back(
                        {"build-plan", "build-plan.json", artifact_hash(build_report)});
                }
                if (request.kind == JobKind::RunPreflight) {
                    result.checkpoints.push_back("run-preflight-ready");
                    result.diagnostics.push_back(
                        {DiagnosticSeverity::Information,
                         "native-run-deferred",
                         "Der gemeinsame Run-Job hat Analyse, Codegen und Buildplan validiert.",
                         "Native Hostausfuehrung wird im Phase-11-Runtime-Scope aktiviert.",
                         std::nullopt});
                }
            }
        }
        require_not_cancelled(cancellation);
        result.state = JobState::Completed;
        notify(observer, request, JobState::Completed, 100u, "completed");
    } catch (const JobState state) {
        result.state = state;
        result.diagnostics.push_back(
            make_error("job-cancelled",
                       "Job wurde kontrolliert abgebrochen.",
                       "Der Job kann mit denselben Eingaben wiederholt werden."));
        notify(observer, request, state, 100u, "cancelled", result.diagnostics.back());
    } catch (const std::exception& error) {
        result.state = JobState::Failed;
        result.diagnostics.push_back(
            make_error("job-failed",
                       error.what(),
                       "Quelle und Projekteinstellungen pruefen und Job wiederholen."));
        notify(observer, request, JobState::Failed, 100u, "failed", result.diagnostics.back());
    }
    const auto result_path = request.output_root / "job-result.json";
    write_atomic(result_path, format_job_result_json(result));
    result.artifacts.push_back({"job-result", "job-result.json", artifact_hash(result_path)});
    return result;
}

JobCoordinator::JobCoordinator(ApplicationService service) : service_(std::move(service)) {}

JobResult JobCoordinator::execute(const JobRequest& request,
                                  const std::shared_ptr<Cancellation>& cancellation,
                                  const JobObserver& observer) {
    const auto output = std::filesystem::absolute(request.output_root).lexically_normal();
    {
        std::scoped_lock lock(mutex_);
        if (std::find(active_outputs_.begin(), active_outputs_.end(), output) !=
            active_outputs_.end())
            throw std::runtime_error("Ein aktiver Job verwendet bereits dasselbe Ausgabeziel.");
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
    case JobState::Failed:
        return "failed";
    case JobState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string redact_sensitive_text(const std::string_view text) {
    std::string result(text);
    const std::string posix_home = std::string("/") + "home" + "/";
    const std::string posix_users = std::string("/") + "Users" + "/";
    for (std::size_t index = 0u; index < result.size();) {
        const bool drive = index + 2u < result.size() &&
                           ((result[index] >= 'A' && result[index] <= 'Z') ||
                            (result[index] >= 'a' && result[index] <= 'z')) &&
                           result[index + 1u] == ':' &&
                           (result[index + 2u] == '\\' || result[index + 2u] == '/');
        const bool home = result.compare(index, posix_home.size(), posix_home) == 0u ||
                          result.compare(index, posix_users.size(), posix_users) == 0u;
        if (!drive && !home) {
            ++index;
            continue;
        }
        auto end = index;
        while (end < result.size() && result[end] != '\n' && result[end] != '\r' &&
               result[end] != '\"' && result[end] != '\'')
            ++end;
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
           << ",\"project_identity\":" << io::quote_json(result.project_identity)
           << ",\"artifacts\":[";
    for (std::size_t index = 0u; index < result.artifacts.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& artifact = result.artifacts[index];
        output << "{\"role\":" << io::quote_json(artifact.role)
               << ",\"path\":" << io::quote_json(artifact.relative_path.generic_string())
               << ",\"sha256\":" << io::quote_json(artifact.sha256) << '}';
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

} // namespace katana::app
