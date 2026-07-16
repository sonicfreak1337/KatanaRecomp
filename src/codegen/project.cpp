#include "katana/codegen/project.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace katana::codegen {
namespace {

constexpr std::string_view artifact_manifest_name = ".katana-generated-artifacts";
constexpr std::string_view artifact_manifest_header = "katana-codegen-artifacts-v1";

std::filesystem::path validate_relative_path(const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    if (normalized.empty() || normalized.is_absolute()) {
        throw std::invalid_argument("Codegen-Projektartefakt braucht einen relativen Pfad.");
    }
    for (const auto& component : normalized) {
        if (component == "..") {
            throw std::invalid_argument("Codegen-Projektartefakt verlaesst das Ausgabeziel.");
        }
    }
    return normalized;
}

void write_file(const std::filesystem::path& path, const std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("Codegen-Projektdatei konnte nicht geoeffnet werden."); }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) { throw std::runtime_error("Codegen-Projektdatei konnte nicht geschrieben werden."); }
}

std::vector<std::filesystem::path> read_artifact_manifest(const std::filesystem::path& root) {
    const auto path = root / artifact_manifest_name;
    if (!std::filesystem::exists(path)) { return {}; }
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line != artifact_manifest_header) {
        throw std::runtime_error("Katana-Artefaktmanifest ist unlesbar oder besitzt eine unbekannte Version.");
    }
    std::vector<std::filesystem::path> paths;
    while (std::getline(input, line)) {
        if (line.empty()) { continue; }
        paths.push_back(validate_relative_path(std::filesystem::path(line)));
    }
    if (!input.eof()) { throw std::runtime_error("Katana-Artefaktmanifest konnte nicht vollstaendig gelesen werden."); }
    std::sort(paths.begin(), paths.end());
    if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) {
        throw std::runtime_error("Katana-Artefaktmanifest enthaelt doppelte Pfade.");
    }
    return paths;
}

std::string artifact_manifest(const std::vector<std::filesystem::path>& paths) {
    std::ostringstream output;
    output << artifact_manifest_header << '\n';
    for (const auto& path : paths) { output << path.generic_string() << '\n'; }
    return output.str();
}

std::string cmake_project(const std::vector<std::filesystem::path>& sources) {
    std::ostringstream output;
    output << "cmake_minimum_required(VERSION 3.25)\n"
           << "project(KatanaGenerated LANGUAGES CXX)\n"
           << "add_library(katana_generated STATIC\n";
    for (const auto& source : sources) { output << "    " << source.generic_string() << '\n'; }
    output << ")\ntarget_compile_features(katana_generated PUBLIC cxx_std_20)\n";
    return output.str();
}

std::string ninja_project(const std::vector<std::filesystem::path>& sources) {
    std::ostringstream output;
    output << "cxx = c++\n"
           << "rule cxx\n  command = $cxx -std=c++20 -c $in -o $out\n"
           << "rule archive\n  command = ar rcs $out $in\n";
    std::vector<std::string> objects;
    for (std::size_t index = 0u; index < sources.size(); ++index) {
        const auto object = "obj/unit-" + std::to_string(index) + ".o";
        objects.push_back(object);
        output << "build " << object << ": cxx " << sources[index].generic_string() << '\n';
    }
    output << "build libkatana_generated.a: archive";
    for (const auto& object : objects) { output << ' ' << object; }
    output << "\ndefault libkatana_generated.a\n";
    return output.str();
}

std::string compile_commands(const std::vector<std::filesystem::path>& sources) {
    std::ostringstream output;
    output << "[\n";
    for (std::size_t index = 0u; index < sources.size(); ++index) {
        const auto path = sources[index].generic_string();
        output << "  {\"directory\":\".\",\"file\":\"" << path
               << "\",\"command\":\"c++ -std=c++20 -c " << path << "\"}"
               << (index + 1u == sources.size() ? "\n" : ",\n");
    }
    output << "]\n";
    return output.str();
}

} // namespace

ProjectWriteResult write_codegen_project(
    const std::filesystem::path& output_root,
    std::vector<ProjectArtifact> artifacts,
    const ProjectWriteOptions& options
) {
    if (output_root.empty() || options.parallel_jobs == 0u) {
        throw std::invalid_argument("Codegen-Projektausgabe braucht Ziel und mindestens einen Job.");
    }
    if ((options.cache == nullptr) != options.cache_key.empty()) {
        throw std::invalid_argument("Codegen-Projektcache braucht Cache und Schluessel gemeinsam.");
    }
    for (auto& artifact : artifacts) {
        artifact.relative_path = validate_relative_path(artifact.relative_path);
    }
    std::sort(artifacts.begin(), artifacts.end(), [](const auto& left, const auto& right) {
        return left.relative_path.generic_string() < right.relative_path.generic_string();
    });
    for (std::size_t index = 1u; index < artifacts.size(); ++index) {
        if (artifacts[index - 1u].relative_path == artifacts[index].relative_path) {
            throw std::invalid_argument("Codegen-Projekt enthaelt einen doppelten Artefaktpfad.");
        }
    }

    const auto root = std::filesystem::absolute(output_root).lexically_normal();
    std::filesystem::create_directories(root);
    const auto previous_files = read_artifact_manifest(root);
    struct WriteOutcome { std::filesystem::path path; bool hit; };
    const auto write_artifact = [&](const ProjectArtifact& artifact) {
        std::string content = artifact.content;
        bool hit = false;
        if (options.cache != nullptr) {
            const auto cache_name = artifact.relative_path.generic_string();
            const auto cached = options.cache->load(options.cache_key, cache_name);
            if (cached) {
                content = *cached;
                hit = true;
            } else {
                options.cache->store(options.cache_key, cache_name, content);
            }
        }
        write_file(root / artifact.relative_path, content);
        return WriteOutcome{artifact.relative_path, hit};
    };

    std::vector<WriteOutcome> outcomes;
    outcomes.reserve(artifacts.size());
    if (options.parallel_jobs == 1u) {
        for (const auto& artifact : artifacts) { outcomes.push_back(write_artifact(artifact)); }
    } else {
        std::vector<std::future<WriteOutcome>> pending;
        pending.reserve(artifacts.size());
        for (const auto& artifact : artifacts) {
            pending.push_back(std::async(std::launch::async, write_artifact, std::cref(artifact)));
            if (pending.size() == options.parallel_jobs) {
                for (auto& future : pending) { outcomes.push_back(future.get()); }
                pending.clear();
            }
        }
        for (auto& future : pending) { outcomes.push_back(future.get()); }
    }

    std::vector<std::filesystem::path> sources;
    for (const auto& artifact : artifacts) {
        if (artifact.relative_path.extension() == ".cpp") { sources.push_back(artifact.relative_path); }
    }
    const std::array build_files = {
        ProjectArtifact{"CMakeLists.txt", cmake_project(sources)},
        ProjectArtifact{"build.ninja", ninja_project(sources)},
        ProjectArtifact{"compile_commands.json", compile_commands(sources)}
    };
    std::vector<std::filesystem::path> current_files;
    current_files.reserve(artifacts.size() + build_files.size());
    for (const auto& artifact : artifacts) { current_files.push_back(artifact.relative_path); }
    for (const auto& build_file : build_files) { current_files.push_back(build_file.relative_path); }
    std::sort(current_files.begin(), current_files.end());

    ProjectWriteResult result;
    std::vector<std::filesystem::path> stale_files;
    std::set_difference(
        previous_files.begin(), previous_files.end(),
        current_files.begin(), current_files.end(),
        std::back_inserter(stale_files)
    );
    for (const auto& relative : stale_files) {
        const auto stale = root / relative;
        std::error_code error;
        const bool removed = std::filesystem::remove(stale, error);
        if (error) {
            throw std::runtime_error(
                "Veraltetes Katana-Artefakt konnte nicht entfernt werden: " +
                relative.generic_string() + (error ? " (" + error.message() + ")" : "")
            );
        }
        if (removed) {
            result.removed_files.push_back(relative);
            auto parent = stale.parent_path();
            while (parent != root) {
                std::error_code directory_error;
                if (!std::filesystem::remove(parent, directory_error) || directory_error) { break; }
                parent = parent.parent_path();
            }
        }
    }
    for (const auto& build_file : build_files) { write_file(root / build_file.relative_path, build_file.content); }
    write_file(root / artifact_manifest_name, artifact_manifest(current_files));

    for (const auto& outcome : outcomes) {
        result.written_files.push_back(outcome.path);
        outcome.hit ? ++result.cache_hits : ++result.cache_misses;
    }
    for (const auto& build_file : build_files) { result.written_files.push_back(build_file.relative_path); }
    result.written_files.emplace_back(artifact_manifest_name);
    std::sort(result.written_files.begin(), result.written_files.end());
    return result;
}

} // namespace katana::codegen
