#include "katana/codegen/project.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace katana::codegen {
namespace {

constexpr std::string_view artifact_manifest_name = ".katana-generated-artifacts";
constexpr std::string_view artifact_manifest_v1_header = "katana-codegen-artifacts-v1";
constexpr std::string_view artifact_manifest_header = "katana-codegen-artifacts-v2";
constexpr std::string_view artifact_manifest_generation_prefix = "generation\t";
constexpr std::size_t maximum_project_cache_artifact_bytes =
    64u * 1024u * 1024u;
constexpr std::uintmax_t maximum_artifact_manifest_bytes =
    16u * 1024u * 1024u;
constexpr std::size_t maximum_artifact_manifest_entries = 131'072u;
constexpr std::size_t maximum_artifact_manifest_path_bytes = 4'096u;
constexpr std::size_t maximum_artifact_manifest_line_bytes =
    maximum_artifact_manifest_path_bytes + 512u;

std::atomic_uint64_t artifact_temp_sequence{1u};

std::uint64_t artifact_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

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
    const auto portable = normalized.generic_string();
    if (!std::ranges::all_of(portable, [](const unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '_' || character == '-' ||
                   character == '.' || character == '/';
        })) {
        throw std::invalid_argument(
            "Codegen-Projektartefakt besitzt keinen portablen Buildgraph-Pfad.");
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

struct ArtifactFileBinding final {
    std::uintmax_t size = 0u;
    std::string token;
    // Stable kernel object identity used only across an atomic rename. It is
    // deliberately not part of the persisted binding comparison because the
    // manifest token also binds timestamps that detect later mutations.
    std::string object_token;

    friend bool operator==(const ArtifactFileBinding& left,
                           const ArtifactFileBinding& right) {
        return left.size == right.size && left.token == right.token;
    }
};

struct ArtifactManifestEntry final {
    std::filesystem::path relative_path;
    std::uintmax_t size = 0u;
    std::string sha256;
    ArtifactFileBinding binding;
};

struct ArtifactManifestState final {
    std::vector<ArtifactManifestEntry> entries;
    std::vector<std::filesystem::path> cleanup_paths;
    std::string generation;
    std::optional<ArtifactFileBinding> manifest_binding;
    bool trusted_v2 = false;
};

struct StableFileHash final {
    ArtifactFileBinding binding;
    std::string sha256;
};

bool valid_digest(const std::string_view value) {
    constexpr std::string_view prefix = "sha256:";
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 64u)
        return false;
    return std::ranges::all_of(value.substr(prefix.size()), [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::optional<std::uintmax_t> parse_unsigned(const std::string_view value) {
    if (value.empty()) return std::nullopt;
    std::uintmax_t parsed = 0u;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed, 10);
    if (error != std::errc{} || end != value.data() + value.size())
        return std::nullopt;
    return parsed;
}

std::optional<ArtifactFileBinding> capture_file_binding(
    const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(),
                                    FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    struct HandleGuard final {
        HANDLE value;
        ~HandleGuard() {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    } guard{handle};

    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(handle,
                                      FileBasicInfo,
                                      &basic,
                                      static_cast<DWORD>(sizeof(basic))) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        return std::nullopt;
    }
    ULARGE_INTEGER size{};
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    std::ostringstream identity;
    identity << "windows-v2|" << info.dwVolumeSerialNumber << '|'
             << info.nFileIndexHigh << '|' << info.nFileIndexLow << '|'
             << size.QuadPart << '|' << basic.CreationTime.QuadPart << '|'
             << basic.LastWriteTime.QuadPart << '|' << basic.ChangeTime.QuadPart;
    std::ostringstream object_identity;
    object_identity << "windows-object-v1|" << info.dwVolumeSerialNumber << '|'
                    << info.nFileIndexHigh << '|' << info.nFileIndexLow << '|'
                    << size.QuadPart << '|' << basic.CreationTime.QuadPart;
    return ArtifactFileBinding{
        static_cast<std::uintmax_t>(size.QuadPart),
        "sha256:" + katana::io::sha256_bytes(identity.str()),
        "sha256:" + katana::io::sha256_bytes(object_identity.str())};
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return std::nullopt;
#if defined(__APPLE__)
    const auto modified_seconds = info.st_mtimespec.tv_sec;
    const auto modified_nanos = info.st_mtimespec.tv_nsec;
    const auto changed_seconds = info.st_ctimespec.tv_sec;
    const auto changed_nanos = info.st_ctimespec.tv_nsec;
#else
    const auto modified_seconds = info.st_mtim.tv_sec;
    const auto modified_nanos = info.st_mtim.tv_nsec;
    const auto changed_seconds = info.st_ctim.tv_sec;
    const auto changed_nanos = info.st_ctim.tv_nsec;
#endif
    std::ostringstream identity;
    identity << "posix-v2|" << static_cast<std::uintmax_t>(info.st_dev) << '|'
             << static_cast<std::uintmax_t>(info.st_ino) << '|'
             << static_cast<std::uintmax_t>(info.st_size) << '|'
             << modified_seconds << '|' << modified_nanos << '|'
             << changed_seconds << '|' << changed_nanos;
    std::ostringstream object_identity;
    object_identity << "posix-object-v1|"
                    << static_cast<std::uintmax_t>(info.st_dev) << '|'
                    << static_cast<std::uintmax_t>(info.st_ino) << '|'
                    << static_cast<std::uintmax_t>(info.st_size);
    return ArtifactFileBinding{
        static_cast<std::uintmax_t>(info.st_size),
        "sha256:" + katana::io::sha256_bytes(identity.str()),
        "sha256:" + katana::io::sha256_bytes(object_identity.str())};
#endif
}

std::optional<StableFileHash> hash_file_stably(const std::filesystem::path& path,
                                               const std::uintmax_t expected_size) {
    constexpr std::size_t hash_buffer_bytes = 64u * 1024u;
    for (unsigned attempt = 0u; attempt != 2u; ++attempt) {
        const auto before = capture_file_binding(path);
        if (!before || before->size != expected_size) return std::nullopt;
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        katana::io::Sha256Accumulator hash;
        std::array<char, hash_buffer_bytes> buffer{};
        std::uintmax_t bytes_read = 0u;
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
                bytes_read += static_cast<std::uintmax_t>(count);
            }
        }
        if (!input.eof() || bytes_read != expected_size) return std::nullopt;
        const auto after = capture_file_binding(path);
        if (after && *before == *after)
            return StableFileHash{*after, "sha256:" + hash.finish()};
    }
    return std::nullopt;
}

const ArtifactManifestEntry* find_manifest_entry(
    const ArtifactManifestState& manifest,
    const std::filesystem::path& relative) {
    const auto found = std::lower_bound(
        manifest.entries.begin(),
        manifest.entries.end(),
        relative,
        [](const ArtifactManifestEntry& entry, const std::filesystem::path& value) {
            return entry.relative_path.generic_string() < value.generic_string();
        });
    return found != manifest.entries.end() && found->relative_path == relative ? &*found : nullptr;
}

std::string manifest_records(const std::vector<ArtifactManifestEntry>& entries) {
    std::ostringstream output;
    for (const auto& entry : entries) {
        const auto encoded = entry.relative_path.generic_string();
        if (encoded.empty() || encoded.size() > maximum_artifact_manifest_path_bytes ||
            !valid_digest(entry.sha256) || !valid_digest(entry.binding.token)) {
            throw std::runtime_error("Katana-Artefaktmanifest enthaelt einen ungueltigen Eintrag.");
        }
        output << encoded << '\t' << entry.size << '\t' << entry.sha256 << '\t'
               << entry.binding.token << '\n';
    }
    return output.str();
}

std::string manifest_generation(const std::vector<ArtifactManifestEntry>& entries) {
    return "sha256:" + katana::io::sha256_bytes(manifest_records(entries));
}

std::string artifact_manifest(const std::vector<ArtifactManifestEntry>& entries) {
    if (entries.size() > maximum_artifact_manifest_entries)
        throw std::runtime_error(
            "Katana-Artefaktmanifest ueberschreitet sein Eintragsbudget.");
    auto sorted_entries = entries;
    std::sort(sorted_entries.begin(),
              sorted_entries.end(),
              [](const auto& left, const auto& right) {
                  return left.relative_path.generic_string() <
                         right.relative_path.generic_string();
              });
    for (std::size_t index = 1u; index < sorted_entries.size(); ++index) {
        if (sorted_entries[index - 1u].relative_path == sorted_entries[index].relative_path)
            throw std::runtime_error("Katana-Artefaktmanifest enthaelt doppelte Pfade.");
    }
    std::ostringstream output;
    const auto records = manifest_records(sorted_entries);
    output << artifact_manifest_header << '\n'
           << artifact_manifest_generation_prefix
           << "sha256:" << katana::io::sha256_bytes(records) << '\n'
           << records;
    auto manifest = output.str();
    if (manifest.size() > maximum_artifact_manifest_bytes)
        throw std::runtime_error(
            "Katana-Artefaktmanifest ueberschreitet sein Bytebudget.");
    return manifest;
}

std::vector<std::string_view> split_manifest_line(const std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0u;
    while (true) {
        const auto separator = line.find('\t', begin);
        fields.push_back(line.substr(begin, separator == std::string_view::npos
                                               ? std::string_view::npos
                                               : separator - begin));
        if (separator == std::string_view::npos) break;
        begin = separator + 1u;
    }
    return fields;
}

ArtifactManifestState read_artifact_manifest(const std::filesystem::path& root) {
    ArtifactManifestState result;
    const auto relative = std::filesystem::path(artifact_manifest_name);
    const auto path = secure_artifact_path(root, relative);
    const auto before = capture_file_binding(path);
    if (!before) return result;
    if (before->size > maximum_artifact_manifest_bytes)
        throw std::runtime_error(
            "Katana-Artefaktmanifest ueberschreitet sein Bytebudget.");
    std::ifstream input(path, std::ios::binary);
    if (!input) return result;
    std::string document(static_cast<std::size_t>(before->size), '\0');
    if (!document.empty()) {
        input.read(document.data(), static_cast<std::streamsize>(document.size()));
    }
    if (!input || input.gcount() != static_cast<std::streamsize>(document.size()))
        return result;
    const auto after = capture_file_binding(path);
    if (!after || *before != *after) return result;
    result.manifest_binding = *after;

    std::istringstream lines(document);
    std::string line;
    if (!std::getline(lines, line)) return result;
    if (line == artifact_manifest_v1_header) {
        // v1 only listed paths. It authenticated neither the manifest nor the
        // files, so it may trigger a cold rewrite but must never authorize
        // deletion of an externally replaced or user-created path.
        return result;
    }
    if (line != artifact_manifest_header) return {};
    if (!std::getline(lines, line) ||
        !line.starts_with(artifact_manifest_generation_prefix))
        return {};
    result.generation = line.substr(artifact_manifest_generation_prefix.size());
    if (!valid_digest(result.generation)) return {};
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        if (line.size() > maximum_artifact_manifest_line_bytes ||
            result.entries.size() >= maximum_artifact_manifest_entries)
            throw std::runtime_error(
                "Katana-Artefaktmanifest ueberschreitet sein Eintragsbudget.");
        const auto fields = split_manifest_line(line);
        if (fields.size() != 4u) return {};
        const auto path_text = fields[0];
        const auto size = parse_unsigned(fields[1]);
        if (path_text.empty() || path_text.size() > maximum_artifact_manifest_path_bytes ||
            !size || !valid_digest(fields[2]) || !valid_digest(fields[3]))
            return {};
        result.entries.push_back({validate_relative_path(std::filesystem::path(path_text)),
                                  *size,
                                  std::string(fields[2]),
                                  ArtifactFileBinding{*size, std::string(fields[3])}});
    }
    if (!lines.eof()) return {};
    std::sort(result.entries.begin(),
              result.entries.end(),
              [](const auto& left, const auto& right) {
                  return left.relative_path.generic_string() <
                         right.relative_path.generic_string();
              });
    for (std::size_t index = 1u; index < result.entries.size(); ++index) {
        if (result.entries[index - 1u].relative_path == result.entries[index].relative_path)
            throw std::runtime_error("Katana-Artefaktmanifest enthaelt doppelte Pfade.");
    }
    result.cleanup_paths.reserve(result.entries.size());
    for (const auto& entry : result.entries) result.cleanup_paths.push_back(entry.relative_path);
    if (manifest_generation(result.entries) != result.generation) return {};
    result.trusted_v2 = true;
    return result;
}

bool manifest_is_still_bound(const std::filesystem::path& root,
                             const ArtifactManifestState& manifest) {
    if (!manifest.manifest_binding) return false;
    const auto path = secure_artifact_path(root, std::filesystem::path(artifact_manifest_name));
    const auto current = capture_file_binding(path);
    return current && *current == *manifest.manifest_binding;
}

std::optional<ArtifactFileBinding> atomic_write_file(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    const std::string_view content) {
    const auto safe_relative = validate_relative_path(relative);
    auto target = secure_artifact_path(root, safe_relative);
    std::filesystem::create_directories(target.parent_path());
    target = secure_artifact_path(root, safe_relative);

    auto temporary_relative = safe_relative;
    temporary_relative +=
        ".katana-tmp-" + std::to_string(artifact_process_id()) + "-" +
        std::to_string(artifact_temp_sequence.fetch_add(1u));
    auto temporary = secure_artifact_path(root, temporary_relative);
    const auto cleanup = [&] {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    };
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Codegen-Projektdatei konnte nicht geoeffnet werden.");
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        output.close();
        if (!output) {
            throw std::runtime_error("Codegen-Projektdatei konnte nicht geschrieben werden.");
        }
        const auto prepared = capture_file_binding(temporary);
        if (!prepared || prepared->size != content.size()) {
            throw std::runtime_error(
                "Codegen-Projektdatei konnte vor der Publikation nicht gebunden werden.");
        }
        target = secure_artifact_path(root, safe_relative);
        temporary = secure_artifact_path(root, temporary_relative);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(),
                         target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Codegen-Projektdatei konnte nicht atomar ersetzt werden");
        }
#else
        std::error_code rename_error;
        std::filesystem::rename(temporary, target, rename_error);
        if (rename_error)
            throw std::system_error(rename_error,
                                    "Codegen-Projektdatei konnte nicht atomar ersetzt werden");
#endif
        // The bytes were hashed from the in-memory authoritative content
        // before this call. Binding the same temp-file identity across the
        // atomic rename excludes a racing same-size replacement without a
        // second full read of every freshly emitted translation unit.
        const auto published = capture_file_binding(target);
        if (!published || prepared->object_token.empty() ||
            published->object_token != prepared->object_token ||
            published->size != prepared->size)
            return std::nullopt;
        return published;
    } catch (...) {
        cleanup();
        throw;
    }
}

std::optional<ArtifactFileBinding> existing_match(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    const std::string_view content,
    const std::string_view expected_sha256,
    const ArtifactManifestState& previous_manifest) {
    const auto path = secure_artifact_path(root, relative);
    const auto binding = capture_file_binding(path);
    if (!binding || binding->size != content.size()) return std::nullopt;
    const auto previous = previous_manifest.trusted_v2
                              ? find_manifest_entry(previous_manifest, relative)
                              : nullptr;
    if (previous != nullptr && previous->size == content.size() &&
        previous->sha256 == expected_sha256 && previous->binding == *binding)
        return *binding;
    const auto current = hash_file_stably(path, static_cast<std::uintmax_t>(content.size()));
    if (current && current->sha256 == expected_sha256) return current->binding;
    return std::nullopt;
}

ArtifactManifestEntry write_file(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    const std::string_view content,
    const ArtifactManifestState& previous_manifest) {
    const auto safe_relative = validate_relative_path(relative);
    const auto expected_sha256 = "sha256:" + katana::io::sha256_bytes(content);
    for (unsigned attempt = 0u; attempt != 2u; ++attempt) {
        if (const auto matched = existing_match(root,
                                                safe_relative,
                                                content,
                                                expected_sha256,
                                                previous_manifest)) {
            const auto observed = capture_file_binding(secure_artifact_path(root, safe_relative));
            if (observed && *observed == *matched)
                return {safe_relative,
                        observed->size,
                        expected_sha256,
                        *observed};
        } else {
            const auto published =
                atomic_write_file(root, safe_relative, content);
            if (published && published->size == content.size())
                return {safe_relative,
                        published->size,
                        expected_sha256,
                        *published};
        }
    }
    throw std::runtime_error("Codegen-Projektdatei konnte nicht stabil gebunden werden.");
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
           << "    set(KATANA_SOURCE_IDENTITY_TRUSTED 0)\n"
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
           << "  elseif(DEFINED KATANA_PORT_BUILD_PROFILE AND\n"
           << "         KATANA_PORT_BUILD_PROFILE STREQUAL \"performance\")\n"
           << "    target_compile_options(katana_generated PRIVATE /O2 /Ob2)\n"
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
           << "  command = cmake -S . -B .ninja-build -G Ninja "
              "-DKATANA_NINJA_STANDALONE=ON\n"
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
    auto previous_manifest = read_artifact_manifest(root);
    if (!manifest_is_still_bound(root, previous_manifest)) {
        // A manifest that changed after parsing cannot authorize either a
        // content skip or stale-file deletion.  Treat it as a cold recovery,
        // even if its parsed v2 records were otherwise valid.
        previous_manifest.entries.clear();
        previous_manifest.cleanup_paths.clear();
        previous_manifest.generation.clear();
        previous_manifest.manifest_binding.reset();
        previous_manifest.trusted_v2 = false;
    }
    const auto previous_files = previous_manifest.cleanup_paths;
    struct WriteOutcome {
        std::filesystem::path path;
        bool hit;
        ArtifactManifestEntry manifest_entry;
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
            if (cached && *cached == artifact.content) {
                auto manifest_entry =
                    write_file(root, artifact.relative_path, *cached, previous_manifest);
                hit = true;
                write_progress.advance(1u);
                return WriteOutcome{artifact.relative_path,
                                    hit,
                                    std::move(manifest_entry)};
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
                auto manifest_entry =
                    write_file(root, artifact.relative_path, artifact.content, previous_manifest);
                write_progress.advance(1u);
                return WriteOutcome{artifact.relative_path,
                                    hit,
                                    std::move(manifest_entry)};
            }
        } else {
            auto manifest_entry =
                write_file(root, artifact.relative_path, artifact.content, previous_manifest);
            write_progress.advance(1u);
            return WriteOutcome{artifact.relative_path,
                                hit,
                                std::move(manifest_entry)};
        }
    };

    std::vector<WriteOutcome> outcomes;
    outcomes.reserve(artifacts.size());
    if (options.parallel_jobs == 1u) {
        for (const auto& artifact : artifacts) {
            outcomes.push_back(write_artifact(artifact));
        }
    } else {
        // Keep one bounded worker set for the whole project instead of
        // repeatedly creating and joining std::async waves. Outcome slots
        // preserve deterministic artifact order independently of scheduling.
        std::vector<std::optional<WriteOutcome>> slots(artifacts.size());
        std::atomic_size_t next_artifact{0u};
        std::atomic_bool cancelled{false};
        std::mutex failure_mutex;
        std::exception_ptr failure;
        const auto worker = [&] {
            while (!cancelled.load(std::memory_order_acquire)) {
                const auto index =
                    next_artifact.fetch_add(1u, std::memory_order_relaxed);
                if (index >= artifacts.size()) return;
                try {
                    slots[index].emplace(write_artifact(artifacts[index]));
                } catch (...) {
                    {
                        std::scoped_lock lock(failure_mutex);
                        if (failure == nullptr) failure = std::current_exception();
                    }
                    cancelled.store(true, std::memory_order_release);
                    return;
                }
            }
        };
        const auto worker_count =
            std::min(options.parallel_jobs, artifacts.size());
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        try {
            for (std::size_t index = 0u; index < worker_count; ++index)
                workers.emplace_back(worker);
        } catch (...) {
            cancelled.store(true, std::memory_order_release);
            for (auto& thread : workers)
                if (thread.joinable()) thread.join();
            throw;
        }
        for (auto& thread : workers) thread.join();
        if (failure != nullptr) std::rethrow_exception(failure);
        for (auto& slot : slots) {
            if (!slot.has_value())
                throw std::runtime_error(
                    "Codegen-Projektworker lieferte kein Artefakt.");
            outcomes.push_back(std::move(*slot));
        }
    }
    std::sort(outcomes.begin(),
              outcomes.end(),
              [](const auto& left, const auto& right) {
                  return left.path.generic_string() < right.path.generic_string();
              });

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
    if (std::adjacent_find(current_files.begin(), current_files.end()) != current_files.end()) {
        throw std::invalid_argument("Codegen-Projekt enthaelt einen doppelten Ausgabeweg.");
    }

    ProjectWriteResult result;
    std::vector<std::filesystem::path> stale_files;
    std::set_difference(previous_files.begin(),
                        previous_files.end(),
                        current_files.begin(),
                        current_files.end(),
                        std::back_inserter(stale_files));
    for (const auto& relative : stale_files) {
        const auto stale = secure_artifact_path(root, relative);
        const auto* const previous_entry =
            find_manifest_entry(previous_manifest, relative);
        const auto current_binding = capture_file_binding(stale);
        if (previous_entry == nullptr || !current_binding ||
            *current_binding != previous_entry->binding) {
            // A stale path is removable only while it is still the exact file
            // published by the authenticated previous generation. Preserve a
            // missing, replaced, or user-created path instead of treating its
            // name alone as deletion authority.
            continue;
        }
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
    std::vector<ArtifactManifestEntry> build_manifest_entries;
    build_manifest_entries.reserve(build_files.size());
    for (const auto& build_file : build_files) {
        build_manifest_entries.push_back(
            write_file(root, build_file.relative_path, build_file.content, previous_manifest));
        write_progress.advance(1u);
    }
    std::vector<ArtifactManifestEntry> manifest_entries;
    manifest_entries.reserve(current_files.size());
    for (auto& outcome : outcomes)
        manifest_entries.push_back(std::move(outcome.manifest_entry));
    for (auto& entry : build_manifest_entries)
        manifest_entries.push_back(std::move(entry));
    if (!atomic_write_file(root,
                           artifact_manifest_name,
                           artifact_manifest(manifest_entries)))
        throw std::runtime_error(
            "Katana-Artefaktmanifest konnte nicht stabil publiziert werden.");
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
