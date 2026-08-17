#include "katana/codegen/project.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace katana::codegen {
namespace {

constexpr std::string_view artifact_manifest_name = ".katana-generated-artifacts";
constexpr std::string_view artifact_manifest_header = "katana-codegen-artifacts-v1";
constexpr std::size_t maximum_project_cache_artifact_bytes =
    64u * 1024u * 1024u;
constexpr std::uintmax_t maximum_artifact_manifest_bytes =
    16u * 1024u * 1024u;
constexpr std::size_t maximum_artifact_manifest_entries = 131'072u;
constexpr std::size_t maximum_artifact_manifest_path_bytes = 4'096u;

bool unsafe_project_link(
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

bool contained_by(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto mismatch =
        std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end());
    return mismatch.first == root.end();
}

std::filesystem::path secure_artifact_path(const std::filesystem::path& canonical_root,
                                           const std::filesystem::path& relative) {
    auto candidate = canonical_root;
    bool missing_prefix = false;
    for (const auto& component : relative) {
        candidate /= component;
        if (missing_prefix) {
            continue;
        }
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(candidate, status_error);
        if ((!status_error && !std::filesystem::exists(status)) ||
            status_error == std::errc::no_such_file_or_directory) {
            missing_prefix = true;
            continue;
        }
        if (!status_error && unsafe_project_link(candidate, status)) {
            throw std::runtime_error("Codegen-Projektpfad enthaelt einen symbolischen Link: " +
                                     relative.generic_string());
        }
        if (status_error) {
            throw std::runtime_error(
                "Codegen-Projektpfad konnte nicht geprueft werden: " + relative.generic_string() +
                " (" + status_error.message() + ")");
        }
        std::error_code canonical_error;
        const auto resolved = std::filesystem::canonical(candidate, canonical_error);
        if (canonical_error || !contained_by(canonical_root, resolved)) {
            throw std::runtime_error("Codegen-Projektpfad verlaesst das kanonische Ausgabeziel: " +
                                     relative.generic_string());
        }
    }
    return candidate;
}

void write_file(const std::filesystem::path& root,
                const std::filesystem::path& relative,
                const std::string_view content) {
    auto path = secure_artifact_path(root, relative);
    std::filesystem::create_directories(path.parent_path());
    path = secure_artifact_path(root, relative);
    if (std::filesystem::is_regular_file(path) &&
        std::filesystem::file_size(path) == content.size()) {
        std::ifstream existing(path, std::ios::binary);
        std::string current(content.size(), '\0');
        existing.read(current.data(), static_cast<std::streamsize>(current.size()));
        if (existing && current == content) return;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Codegen-Projektdatei konnte nicht geoeffnet werden.");
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error("Codegen-Projektdatei konnte nicht geschrieben werden.");
    }
}

std::vector<std::filesystem::path> read_artifact_manifest(const std::filesystem::path& root) {
    const auto relative = std::filesystem::path(artifact_manifest_name);
    const auto path = secure_artifact_path(root, relative);
    if (!std::filesystem::exists(path)) {
        return {};
    }
    std::error_code size_error;
    const auto manifest_bytes =
        std::filesystem::file_size(path, size_error);
    if (size_error ||
        manifest_bytes > maximum_artifact_manifest_bytes)
        throw std::runtime_error(
            "Katana-Artefaktmanifest ueberschreitet sein Bytebudget.");
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line != artifact_manifest_header) {
        throw std::runtime_error(
            "Katana-Artefaktmanifest ist unlesbar oder besitzt eine unbekannte Version.");
    }
    std::vector<std::filesystem::path> paths;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        if (line.size() > maximum_artifact_manifest_path_bytes ||
            paths.size() >= maximum_artifact_manifest_entries)
            throw std::runtime_error(
                "Katana-Artefaktmanifest ueberschreitet sein "
                "Pfadbudget.");
        paths.push_back(validate_relative_path(std::filesystem::path(line)));
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "Katana-Artefaktmanifest konnte nicht vollstaendig gelesen werden.");
    }
    std::sort(paths.begin(), paths.end());
    if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) {
        throw std::runtime_error("Katana-Artefaktmanifest enthaelt doppelte Pfade.");
    }
    return paths;
}

std::string artifact_manifest(const std::vector<std::filesystem::path>& paths) {
    if (paths.size() > maximum_artifact_manifest_entries)
        throw std::runtime_error(
            "Katana-Artefaktmanifest ueberschreitet sein Eintragsbudget.");
    std::ostringstream output;
    output << artifact_manifest_header << '\n';
    for (const auto& path : paths) {
        const auto encoded = path.generic_string();
        if (encoded.size() > maximum_artifact_manifest_path_bytes)
            throw std::runtime_error(
                "Katana-Artefaktmanifest enthaelt einen zu langen Pfad.");
        output << encoded << '\n';
    }
    auto manifest = output.str();
    if (manifest.size() > maximum_artifact_manifest_bytes)
        throw std::runtime_error(
            "Katana-Artefaktmanifest ueberschreitet sein Bytebudget.");
    return manifest;
}

std::string cmake_project(const std::vector<std::filesystem::path>& sources) {
    std::ostringstream output;
    output << "cmake_minimum_required(VERSION 3.25)\n"
           << "project(KatanaGenerated LANGUAGES CXX)\n"
           << "set(KATANA_HOST_COMPILE_JOBS \"1\" CACHE STRING "
              "\"Hard host compiler process budget\")\n"
           << "set(KATANA_AOT_COMPILE_JOBS \"\" CACHE STRING "
              "\"Heavy generated-AOT compiler process budget\")\n"
           << "set(KATANA_AOT_HOT_SOURCES \"\" CACHE STRING "
              "\"Measured generated-AOT sources that receive product optimization\")\n"
           << "set(KATANA_PERSISTENT_COMPILER_CACHE_ACTIVE OFF CACHE BOOL "
              "\"Whether the instrumented compiler launcher chains to a persistent cache\")\n"
           << "set(KATANA_PERSISTENT_COMPILER_CACHE_USE_PCH OFF CACHE BOOL "
              "\"Use a generated-AOT PCH even though it prevents MSVC object caching\")\n"
           << "set(KATANA_HOST_ARCHIVE_LAUNCHER \"\" CACHE STRING "
              "\"Instrumented static-archive launcher\")\n"
           << "if(NOT KATANA_HOST_COMPILE_JOBS MATCHES \"^[1-9][0-9]*$\" OR\n"
           << "   KATANA_HOST_COMPILE_JOBS GREATER 256)\n"
           << "  message(FATAL_ERROR \"KATANA_HOST_COMPILE_JOBS must be 1..256\")\n"
           << "endif()\n"
           << "if(KATANA_AOT_COMPILE_JOBS STREQUAL \"\")\n"
           << "  set(KATANA_AOT_COMPILE_JOBS \"${KATANA_HOST_COMPILE_JOBS}\")\n"
           << "  if(MSVC AND KATANA_AOT_COMPILE_JOBS GREATER 24)\n"
           << "    set(KATANA_AOT_COMPILE_JOBS \"24\")\n"
           << "  endif()\n"
           << "endif()\n"
           << "if(NOT KATANA_AOT_COMPILE_JOBS MATCHES \"^[1-9][0-9]*$\" OR\n"
           << "   KATANA_AOT_COMPILE_JOBS GREATER KATANA_HOST_COMPILE_JOBS)\n"
           << "  message(FATAL_ERROR "
              "\"KATANA_AOT_COMPILE_JOBS must be 1..KATANA_HOST_COMPILE_JOBS\")\n"
           << "endif()\n"
           << "set(KATANA_RUNTIME_ROOT \"\" CACHE PATH \"KatanaRecomp source root\")\n"
           << "if(KATANA_RUNTIME_ROOT STREQUAL \"\" AND NOT \"$ENV{KATANA_RUNTIME_ROOT}\" "
              "STREQUAL \"\")\n"
           << "  file(TO_CMAKE_PATH \"$ENV{KATANA_RUNTIME_ROOT}\" KATANA_RUNTIME_ROOT)\n"
           << "endif()\n"
           << "set(KATANA_GENERATED_RUNTIME_TARGET \"\")\n"
           << "if(DEFINED KATANA_PORT_RUNTIME_TARGET AND "
              "TARGET \"${KATANA_PORT_RUNTIME_TARGET}\")\n"
           << "  set(KATANA_GENERATED_RUNTIME_TARGET \"${KATANA_PORT_RUNTIME_TARGET}\")\n"
           << "else()\n"
           << "  if(KATANA_RUNTIME_ROOT STREQUAL \"\" AND\n"
           << "     NOT TARGET KatanaRecomp::native_port_runtime AND "
              "NOT TARGET katana_native_port_runtime)\n"
           << "    find_package(KatanaRecomp CONFIG QUIET)\n"
           << "  endif()\n"
           << "  if(TARGET KatanaRecomp::native_port_runtime)\n"
           << "    set(KATANA_GENERATED_RUNTIME_TARGET "
              "KatanaRecomp::native_port_runtime)\n"
           << "  elseif(TARGET katana_native_port_runtime)\n"
           << "    set(KATANA_GENERATED_RUNTIME_TARGET "
              "katana_native_port_runtime)\n"
           << "  elseif(KATANA_RUNTIME_ROOT STREQUAL \"\")\n"
           << "    message(FATAL_ERROR \"Find KatanaRecomp or set KATANA_RUNTIME_ROOT\")\n"
           << "  else()\n"
           << "    include(\"${KATANA_RUNTIME_ROOT}/cmake/KatanaVersions.cmake\")\n"
           << "    file(MAKE_DIRECTORY "
              "\"${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana\")\n"
           << "    configure_file(\n"
           << "      \"${KATANA_RUNTIME_ROOT}/include/katana/build_contract.hpp.in\"\n"
           << "      \"${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana/"
              "build_contract.hpp\"\n"
           << "      @ONLY\n"
           << "    )\n"
           << "  endif()\n"
           << "endif()\n"
           << "set(KATANA_GENERATED_SOURCES\n";
    for (const auto& source : sources) {
        output << "    " << source.generic_string() << '\n';
    }
    output << ")\n"
           << "set(KATANA_AOT_HOT_SOURCES_EFFECTIVE \"\")\n"
           << "set(KATANA_AOT_HOT_SOURCES_SEEN \"\")\n"
           << "foreach(KATANA_AOT_HOT_SOURCE IN LISTS KATANA_AOT_HOT_SOURCES)\n"
           << "  list(FIND KATANA_GENERATED_SOURCES "
              "\"${KATANA_AOT_HOT_SOURCE}\" KATANA_AOT_HOT_SOURCE_INDEX)\n"
           << "  if(KATANA_AOT_HOT_SOURCE_INDEX EQUAL -1)\n"
           << "    message(STATUS "
              "\"Ignoring stale KATANA_AOT_HOT_SOURCES entry: "
              "${KATANA_AOT_HOT_SOURCE}\")\n"
           << "    continue()\n"
           << "  endif()\n"
           << "  list(FIND KATANA_AOT_HOT_SOURCES_SEEN "
              "\"${KATANA_AOT_HOT_SOURCE}\" KATANA_AOT_HOT_SOURCE_SEEN_INDEX)\n"
           << "  if(NOT KATANA_AOT_HOT_SOURCE_SEEN_INDEX EQUAL -1)\n"
           << "    message(FATAL_ERROR "
              "\"KATANA_AOT_HOT_SOURCES contains a duplicate source: "
              "${KATANA_AOT_HOT_SOURCE}\")\n"
           << "  endif()\n"
           << "  list(APPEND KATANA_AOT_HOT_SOURCES_SEEN "
              "\"${KATANA_AOT_HOT_SOURCE}\")\n"
           << "  list(APPEND KATANA_AOT_HOT_SOURCES_EFFECTIVE "
              "\"${KATANA_AOT_HOT_SOURCE}\")\n"
           << "endforeach()\n"
           << "list(LENGTH KATANA_AOT_HOT_SOURCES_EFFECTIVE "
              "KATANA_AOT_HOT_SOURCE_COUNT)\n"
           << "if(KATANA_AOT_HOT_SOURCE_COUNT GREATER 64)\n"
           << "  message(FATAL_ERROR "
              "\"KATANA_AOT_HOT_SOURCES exceeds the 64-source budget\")\n"
           << "endif()\n"
           << "add_library(katana_generated STATIC ${KATANA_GENERATED_SOURCES})\n"
           << "target_compile_features(katana_generated PUBLIC cxx_std_20)\n"
           << "if(CMAKE_GENERATOR MATCHES \"Ninja\")\n"
           << "  set_property(GLOBAL APPEND PROPERTY JOB_POOLS\n"
           << "    katana_aot_compile_pool=${KATANA_AOT_COMPILE_JOBS})\n"
           << "  set_property(TARGET katana_generated PROPERTY\n"
           << "    JOB_POOL_COMPILE katana_aot_compile_pool)\n"
           << "endif()\n"
           << "if(NOT KATANA_HOST_ARCHIVE_LAUNCHER STREQUAL \"\" AND\n"
           << "   NOT CMAKE_GENERATOR MATCHES \"^Visual Studio\")\n"
           << "  set_property(TARGET katana_generated PROPERTY "
              "RULE_LAUNCH_LINK \"${KATANA_HOST_ARCHIVE_LAUNCHER}\")\n"
           << "endif()\n"
           << "# MSVC emits /Fp for every PCH consumer. sccache deliberately marks\n"
           << "# those invocations non-cacheable, so persistent-cache exports keep\n"
           << "# each generated object independently content-addressable. A caller\n"
           << "# may opt back into PCH only for an explicitly cacheless cold build.\n"
           << "if(NOT MSVC OR NOT KATANA_PERSISTENT_COMPILER_CACHE_ACTIVE OR\n"
           << "   KATANA_PERSISTENT_COMPILER_CACHE_USE_PCH)\n"
           << "if(DEFINED KATANA_PORT_RUNTIME_PROFILE AND\n"
           << "   KATANA_PORT_RUNTIME_PROFILE STREQUAL \"native-port\")\n"
           << "  target_precompile_headers(katana_generated PRIVATE\n"
           << "    <katana/runtime/native_port_aot_runtime.hpp>\n"
           << "  )\n"
           << "else()\n"
           << "  target_precompile_headers(katana_generated PRIVATE\n"
           << "    <katana/runtime/aot_runtime_abi.hpp>\n"
           << "  )\n"
           << "endif()\n"
           << "endif()\n"
           << "if(MSVC)\n"
           << "  target_compile_options(katana_generated PRIVATE /bigobj)\n"
           << "  set_property(TARGET katana_generated PROPERTY\n"
           << "    MSVC_DEBUG_INFORMATION_FORMAT\n"
           << "    \"$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>\")\n"
           << "  if(DEFINED KATANA_PORT_BUILD_PROFILE AND\n"
           << "     KATANA_PORT_BUILD_PROFILE STREQUAL \"bringup\")\n"
           << "    target_compile_options(katana_generated PRIVATE /O1 /Ob0)\n"
           << "    foreach(KATANA_AOT_HOT_SOURCE IN LISTS "
              "KATANA_AOT_HOT_SOURCES_EFFECTIVE)\n"
           << "      set_property(SOURCE \"${KATANA_AOT_HOT_SOURCE}\" APPEND "
              "PROPERTY COMPILE_OPTIONS /O2 /Ob2)\n"
           << "      set_property(SOURCE \"${KATANA_AOT_HOT_SOURCE}\" PROPERTY "
              "SKIP_PRECOMPILE_HEADERS ON)\n"
           << "    endforeach()\n"
           << "  endif()\n"
           << "  if(CMAKE_GENERATOR MATCHES \"^Visual Studio\")\n"
           << "    target_compile_options(katana_generated PRIVATE "
              "/MP${KATANA_AOT_COMPILE_JOBS})\n"
           << "  endif()\n"
           << "endif()\n"
           << "if(NOT KATANA_GENERATED_RUNTIME_TARGET STREQUAL \"\")\n"
           << "  target_link_libraries(katana_generated PRIVATE "
              "${KATANA_GENERATED_RUNTIME_TARGET})\n"
           << "else()\n"
           << "  target_include_directories(katana_generated PRIVATE\n"
           << "    \"${KATANA_RUNTIME_ROOT}/include\"\n"
           << "    \"${CMAKE_CURRENT_BINARY_DIR}/generated/include\"\n"
           << "  )\n"
           << "endif()\n"
           << "if(KATANA_NINJA_STANDALONE)\n"
           << "  set_target_properties(katana_generated PROPERTIES\n"
           << "    ARCHIVE_OUTPUT_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}\"\n"
           << "    ARCHIVE_OUTPUT_DIRECTORY_DEBUG \"${CMAKE_CURRENT_SOURCE_DIR}\"\n"
           << "    ARCHIVE_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_CURRENT_SOURCE_DIR}\"\n"
           << "    ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO \"${CMAKE_CURRENT_SOURCE_DIR}\"\n"
           << "    ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL \"${CMAKE_CURRENT_SOURCE_DIR}\"\n"
           << "    PREFIX \"lib\"\n"
           << "    SUFFIX \".a\"\n"
           << "  )\n"
           << "endif()\n";
    return output.str();
}

std::string ninja_project(const std::vector<std::filesystem::path>& sources) {
    std::ostringstream output;
    output << "ninja_required_version = 1.10\n"
           << "rule configure\n"
           << "  command = cmake -S . -B .ninja-build -DKATANA_NINJA_STANDALONE=ON\n"
           << "  description = Configure Katana generated archive\n"
           << "  generator = 1\n"
           << "rule archive\n"
           << "  command = cmake --build .ninja-build --target katana_generated\n"
           << "  description = Build Katana generated archive\n"
           << "build .ninja-build/CMakeCache.txt: configure CMakeLists.txt\n"
           << "build force: phony\n"
           << "build libkatana_generated.a: archive force";
    for (const auto& source : sources)
        output << ' ' << source.generic_string();
    output << " | .ninja-build/CMakeCache.txt\ndefault libkatana_generated.a\n";
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

ProjectWriteResult write_codegen_project(const std::filesystem::path& output_root,
                                         std::vector<ProjectArtifact> artifacts,
                                         const ProjectWriteOptions& options) {
    auto write_progress = options.progress.begin(
        katana::ProgressOperation::ArtifactWrite,
        katana::ProgressUnit::Files,
        std::nullopt,
        "codegen-project");
    if (output_root.empty() || options.parallel_jobs == 0u) {
        throw std::invalid_argument(
            "Codegen-Projektausgabe braucht Ziel und mindestens einen Job.");
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

    const auto absolute_root = std::filesystem::absolute(output_root).lexically_normal();
    std::error_code root_status_error;
    const auto root_status = std::filesystem::symlink_status(absolute_root, root_status_error);
    if (!root_status_error &&
        std::filesystem::exists(root_status) &&
        unsafe_project_link(absolute_root, root_status)) {
        throw std::runtime_error("Codegen-Ausgabeziel darf kein symbolischer Link sein.");
    }
    if (root_status_error && root_status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("Codegen-Ausgabeziel konnte nicht geprueft werden: " +
                                 root_status_error.message());
    }
    std::filesystem::create_directories(absolute_root);
    const auto root = std::filesystem::canonical(absolute_root);
    const auto previous_files = read_artifact_manifest(root);
    struct WriteOutcome {
        std::filesystem::path path;
        bool hit;
    };
    const auto write_artifact = [&](const ProjectArtifact& artifact) {
        bool hit = false;
        if (options.cache != nullptr &&
            artifact.content.size() <=
                maximum_project_cache_artifact_bytes) {
            const auto cache_name = artifact.relative_path.generic_string();
            std::optional<std::string> cached;
            try {
                cached = options.cache->load_integrity_bounded(
                    options.cache_key,
                    cache_name,
                    maximum_project_cache_artifact_bytes);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception&) {
                // The project cache is optional. Unsafe local state is a miss.
            }
            if (cached) {
                write_file(root, artifact.relative_path, *cached);
                hit = true;
            } else {
                try {
                    options.cache->store_integrity_bounded(
                        options.cache_key,
                        cache_name,
                        artifact.content,
                        maximum_project_cache_artifact_bytes);
                } catch (const std::bad_alloc&) {
                    throw;
                } catch (const std::exception&) {
                    // Fresh generated content remains authoritative even if
                    // optional cache publication is unavailable.
                }
                write_file(root, artifact.relative_path, artifact.content);
            }
        } else {
            write_file(root, artifact.relative_path, artifact.content);
        }
        write_progress.advance(1u);
        return WriteOutcome{artifact.relative_path, hit};
    };

    std::vector<WriteOutcome> outcomes;
    outcomes.reserve(artifacts.size());
    if (options.parallel_jobs == 1u) {
        for (const auto& artifact : artifacts) {
            outcomes.push_back(write_artifact(artifact));
        }
    } else {
        std::vector<std::future<WriteOutcome>> pending;
        pending.reserve(artifacts.size());
        for (const auto& artifact : artifacts) {
            pending.push_back(std::async(std::launch::async, write_artifact, std::cref(artifact)));
            if (pending.size() == options.parallel_jobs) {
                for (auto& future : pending) {
                    outcomes.push_back(future.get());
                }
                pending.clear();
            }
        }
        for (auto& future : pending) {
            outcomes.push_back(future.get());
        }
    }

    struct BuildSource {
        std::filesystem::path path;
        std::size_t content_size = 0u;
    };
    std::vector<BuildSource> scheduled_sources;
    for (const auto& artifact : artifacts) {
        // Host-side validation tools are emitted into the artifact set so
        // the root product project can build them, but they must not also be
        // compiled into the generated guest-code archive.
        if (artifact.relative_path.extension() == ".cpp" &&
            !artifact.relative_path.generic_string().starts_with("tools/")) {
            scheduled_sources.push_back({artifact.relative_path, artifact.content.size()});
        }
    }
    // Ninja starts ready compile edges in target-source order. Schedule the largest generated
    // units first so long-running compiler jobs overlap instead of waiting behind small dispatch
    // shards. Path order remains the deterministic tie breaker.
    std::sort(scheduled_sources.begin(),
              scheduled_sources.end(),
              [](const auto& left, const auto& right) {
                  if (left.content_size != right.content_size)
                      return left.content_size > right.content_size;
                  return left.path.generic_string() < right.path.generic_string();
              });
    std::vector<std::filesystem::path> sources;
    sources.reserve(scheduled_sources.size());
    for (auto& source : scheduled_sources)
        sources.push_back(std::move(source.path));
    const std::array build_files = {
        ProjectArtifact{"CMakeLists.txt", cmake_project(sources)},
        ProjectArtifact{"build.ninja", ninja_project(sources)},
        ProjectArtifact{"compile_commands.json", compile_commands(sources)}};
    std::vector<std::filesystem::path> current_files;
    current_files.reserve(artifacts.size() + build_files.size());
    for (const auto& artifact : artifacts) {
        current_files.push_back(artifact.relative_path);
    }
    for (const auto& build_file : build_files) {
        current_files.push_back(build_file.relative_path);
    }
    std::sort(current_files.begin(), current_files.end());

    ProjectWriteResult result;
    std::vector<std::filesystem::path> stale_files;
    std::set_difference(previous_files.begin(),
                        previous_files.end(),
                        current_files.begin(),
                        current_files.end(),
                        std::back_inserter(stale_files));
    for (const auto& relative : stale_files) {
        const auto stale = secure_artifact_path(root, relative);
        std::error_code error;
        const bool removed = std::filesystem::remove(stale, error);
        if (error) {
            throw std::runtime_error("Veraltetes Katana-Artefakt konnte nicht entfernt werden: " +
                                     relative.generic_string() +
                                     (error ? " (" + error.message() + ")" : ""));
        }
        if (removed) {
            result.removed_files.push_back(relative);
            write_progress.advance(1u);
            auto parent = stale.parent_path();
            while (parent != root) {
                std::error_code directory_error;
                if (!std::filesystem::remove(parent, directory_error) || directory_error) {
                    break;
                }
                parent = parent.parent_path();
            }
        }
    }
    for (const auto& build_file : build_files) {
        write_file(root, build_file.relative_path, build_file.content);
        write_progress.advance(1u);
    }
    write_file(root, artifact_manifest_name, artifact_manifest(current_files));
    write_progress.advance(1u);

    for (const auto& outcome : outcomes) {
        result.written_files.push_back(outcome.path);
        outcome.hit ? ++result.cache_hits : ++result.cache_misses;
    }
    for (const auto& build_file : build_files) {
        result.written_files.push_back(build_file.relative_path);
    }
    result.written_files.emplace_back(artifact_manifest_name);
    std::sort(result.written_files.begin(), result.written_files.end());
    katana::ProgressCounterSnapshot counters;
    counters.cache_hits = result.cache_hits;
    counters.cache_misses = result.cache_misses;
    write_progress.update(std::move(counters));
    write_progress.complete();
    return result;
}

} // namespace katana::codegen
