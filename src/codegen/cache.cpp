#include "katana/codegen/cache.hpp"

#include "katana/io/input_provenance.hpp"

#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace katana::codegen {
namespace {

constexpr std::size_t maximum_portable_cache_path_characters = 220u;
constexpr std::size_t maximum_legacy_codegen_cache_artifact_bytes =
    256u * 1024u * 1024u;
constexpr std::string_view integrity_artifact_magic{
    "KATANA-CACHE-ARTIFACT-V2\n"};
constexpr std::size_t integrity_artifact_size_digits = 20u;
constexpr std::size_t integrity_artifact_sha256_characters = 64u;
constexpr std::size_t integrity_artifact_overhead =
    integrity_artifact_magic.size() + integrity_artifact_size_digits + 1u +
    integrity_artifact_sha256_characters + 1u;

std::size_t integrity_artifact_budget(
    const std::size_t maximum_payload_bytes) {
    if (maximum_payload_bytes == 0u ||
        maximum_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                integrity_artifact_overhead)
        throw std::invalid_argument(
            "Integritaetsgebundener Cache besitzt ein ungueltiges "
            "Payloadbudget.");
    return maximum_payload_bytes + integrity_artifact_overhead;
}

void append_field(std::ostringstream& output, const std::string_view value) {
    output << value.size() << ':' << value << ';';
}

std::string integrity_artifact_digest(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::string_view content) {
    std::ostringstream binding;
    append_field(binding, "katana-cache-integrity-v2");
    append_field(binding, key);
    append_field(binding, artifact_name);
    append_field(binding, content);
    return katana::io::sha256_bytes(binding.str());
}

std::string integrity_artifact_envelope(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::string_view content) {
    std::ostringstream output;
    output << integrity_artifact_magic
           << std::setw(
                  static_cast<int>(
                      integrity_artifact_size_digits))
           << std::setfill('0') << content.size() << '\n'
           << integrity_artifact_digest(key, artifact_name, content) << '\n'
           << content;
    return output.str();
}

std::optional<std::string> parse_integrity_artifact(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::string_view artifact,
    const std::size_t maximum_payload_bytes) {
    if (artifact.size() < integrity_artifact_overhead ||
        !artifact.starts_with(integrity_artifact_magic))
        return std::nullopt;
    auto cursor = integrity_artifact_magic.size();
    std::size_t payload_size = 0u;
    for (std::size_t index = 0u;
         index < integrity_artifact_size_digits;
         ++index) {
        const auto character = artifact[cursor + index];
        if (character < '0' || character > '9')
            return std::nullopt;
        const auto digit =
            static_cast<std::size_t>(character - '0');
        if (payload_size >
            (std::numeric_limits<std::size_t>::max() - digit) /
                10u)
            return std::nullopt;
        payload_size = payload_size * 10u + digit;
    }
    cursor += integrity_artifact_size_digits;
    if (artifact[cursor++] != '\n')
        return std::nullopt;
    const auto stored_sha256 = artifact.substr(
        cursor, integrity_artifact_sha256_characters);
    cursor += integrity_artifact_sha256_characters;
    if (artifact[cursor++] != '\n' ||
        payload_size > maximum_payload_bytes ||
        payload_size != artifact.size() - cursor)
        return std::nullopt;
    const auto payload = artifact.substr(cursor, payload_size);
    if (integrity_artifact_digest(key, artifact_name, payload) !=
        stored_sha256)
        return std::nullopt;
    return std::string(payload);
}

bool safe_component(const std::string_view value) noexcept {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    for (const auto character : value) {
        const bool accepted = (character >= 'a' && character <= 'z') ||
                              (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9') || character == '-' ||
                              character == '_' || character == '.';
        if (!accepted) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool unsafe_cache_link(
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

[[nodiscard]] bool safe_cache_directory_chain(
    const std::filesystem::path& root,
    const std::filesystem::path& directory) {
    const auto relative = directory.lexically_relative(root);
    if ((relative.empty() && directory != root) || relative.is_absolute() ||
        (!relative.empty() && *relative.begin() == ".."))
        return false;
    std::error_code error;
    auto status = std::filesystem::symlink_status(root, error);
    if (error || !std::filesystem::is_directory(status) ||
        unsafe_cache_link(root, status))
        return false;
    auto current = root;
    for (const auto& component : relative) {
        if (component.empty() || component == ".") continue;
        current /= component;
        status = std::filesystem::symlink_status(current, error);
        if (error || !std::filesystem::is_directory(status) ||
            unsafe_cache_link(current, status))
            return false;
    }
    return true;
}

void ensure_safe_cache_directory(const std::filesystem::path& root,
                                  const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error)
        throw std::filesystem::filesystem_error(
            "Begrenztes Codegen-Cacheverzeichnis konnte nicht erstellt werden.",
            root,
            error);
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error || !std::filesystem::is_directory(root_status) ||
        unsafe_cache_link(root, root_status))
        throw std::runtime_error(
            "Begrenzter Codegen-Cache besitzt kein sicheres Stammverzeichnis.");

    const auto relative = directory.lexically_relative(root);
    if (relative.empty() && directory != root)
        throw std::runtime_error(
            "Begrenzter Codegen-Cachepfad liegt ausserhalb seines Stamms.");
    if (relative.is_absolute() ||
        (!relative.empty() && *relative.begin() == ".."))
        throw std::runtime_error(
            "Begrenzter Codegen-Cachepfad liegt ausserhalb seines Stamms.");
    auto current = root;
    for (const auto& component : relative) {
        if (component.empty() || component == ".") continue;
        current /= component;
        auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error &&
             status.type() ==
                 std::filesystem::file_type::not_found)) {
            error.clear();
            if (!std::filesystem::create_directory(current, error) && error)
                throw std::filesystem::filesystem_error(
                    "Begrenztes Codegen-Cacheverzeichnis konnte nicht erstellt werden.",
                    current,
                    error);
            status = std::filesystem::symlink_status(current, error);
        }
        if (error || !std::filesystem::is_directory(status) ||
            unsafe_cache_link(current, status))
            throw std::runtime_error(
                "Begrenzter Codegen-Cachepfad ist kein sicherer Ordner.");
    }
}

} // namespace

std::string make_codegen_cache_key(const CodegenCacheInputs& inputs) {
    if (inputs.input_hash.empty() || inputs.ir_hash.empty() || inputs.configuration_hash.empty() ||
        inputs.backend_name.empty() || inputs.backend_abi == 0u || inputs.runtime_abi == 0u ||
        inputs.manifest_hash.empty() || inputs.overrides_hash.empty() || inputs.ir_version == 0u ||
        inputs.optimization_version == 0u || inputs.tool_version.empty() ||
        inputs.implementation_identity.empty()) {
        throw std::invalid_argument("Codegen-Cache-Schluessel ist unvollstaendig.");
    }
    std::ostringstream canonical;
    append_field(canonical, std::to_string(codegen_cache_schema_version));
    append_field(canonical, inputs.input_hash);
    append_field(canonical, inputs.ir_hash);
    append_field(canonical, inputs.configuration_hash);
    append_field(canonical, inputs.backend_name);
    append_field(canonical, std::to_string(inputs.backend_abi));
    append_field(canonical, std::to_string(inputs.runtime_abi));
    append_field(canonical, inputs.manifest_hash);
    append_field(canonical, inputs.overrides_hash);
    append_field(canonical, std::to_string(inputs.ir_version));
    append_field(canonical, std::to_string(inputs.optimization_version));
    append_field(canonical, inputs.tool_version);
    append_field(canonical, inputs.implementation_identity);
    return "cg-v" + std::to_string(codegen_cache_schema_version) + '-' +
           katana::io::sha256_bytes(canonical.str());
}

CodegenCache::CodegenCache(std::filesystem::path root) : root_(std::move(root)) {
    if (root_.empty()) {
        throw std::invalid_argument("Codegen-Cache braucht ein Stammverzeichnis.");
    }
    root_ = std::filesystem::absolute(root_).lexically_normal();
}

std::optional<std::string> CodegenCache::load(const std::string_view key,
                                              const std::string_view artifact_name) const {
    return load_bounded(
        key,
        artifact_name,
        maximum_legacy_codegen_cache_artifact_bytes);
}

std::optional<std::string>
CodegenCache::load_bounded(const std::string_view key,
                           const std::string_view artifact_name,
                           const std::size_t maximum_bytes) const {
    if (maximum_bytes == 0u)
        throw std::invalid_argument(
            "Begrenzter Codegen-Cache-Read braucht ein Bytebudget.");
    const auto path = artifact_path(key, artifact_name);
    if (!safe_cache_directory_chain(root_, path.parent_path()))
        return std::nullopt;
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() ==
             std::filesystem::file_type::not_found))
        return std::nullopt;
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_cache_link(path, status))
        return std::nullopt;
    const auto file_bytes = std::filesystem::file_size(path, status_error);
    if (status_error || file_bytes > maximum_bytes ||
        file_bytes >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max()))
        return std::nullopt;

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 ||
        static_cast<std::uintmax_t>(input.tellg()) != file_bytes)
        return std::nullopt;
    std::string content(static_cast<std::size_t>(file_bytes), '\0');
    input.seekg(0, std::ios::beg);
    if (!content.empty())
        input.read(content.data(),
                   static_cast<std::streamsize>(content.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        return std::nullopt;
    input.close();

    const auto final_status =
        std::filesystem::symlink_status(path, status_error);
    if (!safe_cache_directory_chain(root_, path.parent_path()) ||
        status_error || !std::filesystem::is_regular_file(final_status) ||
        unsafe_cache_link(path, final_status) ||
        std::filesystem::file_size(path, status_error) != file_bytes ||
        status_error)
        return std::nullopt;
    return content;
}

std::optional<std::string>
CodegenCache::load_integrity_bounded(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::size_t maximum_payload_bytes) const {
    const auto maximum_artifact_bytes =
        integrity_artifact_budget(maximum_payload_bytes);
    const auto artifact =
        load_bounded(
            key, artifact_name, maximum_artifact_bytes);
    if (!artifact)
        return std::nullopt;
    return parse_integrity_artifact(
        key, artifact_name, *artifact, maximum_payload_bytes);
}

void CodegenCache::store_bounded(const std::string_view key,
                                 const std::string_view artifact_name,
                                 const std::string_view content,
                                 const std::size_t maximum_bytes) {
    if (maximum_bytes == 0u || content.size() > maximum_bytes ||
        content.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max()))
        throw std::invalid_argument(
            "Begrenzter Codegen-Cache-Publish besitzt ein ungueltiges Bytebudget.");
    const auto path = artifact_path(key, artifact_name);
    if (const auto existing =
            load_bounded(key, artifact_name, maximum_bytes);
        existing) {
        if (*existing == content) return;
        throw std::runtime_error(
            "Begrenzter Codegen-Cache-Schluessel kollidiert mit "
            "abweichendem Inhalt.");
    }
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(path, status_error);
    const bool missing =
        status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() ==
             std::filesystem::file_type::not_found);
    if (!missing) {
        const bool safe_oversized_artifact =
            !status_error &&
            std::filesystem::is_regular_file(status) &&
            !unsafe_cache_link(path, status) &&
            safe_cache_directory_chain(root_, path.parent_path());
        const auto existing_bytes =
            safe_oversized_artifact
                ? std::filesystem::file_size(path, status_error)
                : 0u;
        if (!safe_oversized_artifact || status_error ||
            existing_bytes <= maximum_bytes ||
            !std::filesystem::remove(path, status_error) ||
            status_error)
            throw std::runtime_error(
                "Begrenzter Codegen-Cache verweigert ein unsicheres "
                "bestehendes Artefakt.");
    }
    status_error.clear();
    ensure_safe_cache_directory(root_, path.parent_path());
    if (!safe_cache_directory_chain(root_, path.parent_path()))
        throw std::runtime_error(
            "Begrenzter Codegen-Cachepfad wurde vor dem Publish unsicher.");

    std::filesystem::path staging;
    std::random_device random;
    for (std::size_t attempt = 0u; attempt < 32u; ++attempt) {
        staging = root_ /
                  (".publish-bounded-" + std::to_string(random()) + '-' +
                   std::to_string(random()));
        std::error_code create_error;
        if (std::filesystem::create_directory(staging, create_error)) break;
        staging.clear();
    }
    if (staging.empty())
        throw std::runtime_error(
            "Begrenztes Codegen-Cache-Staging konnte nicht atomar "
            "angelegt werden.");
    {
        std::error_code staging_error;
        const auto staging_status =
            std::filesystem::symlink_status(staging, staging_error);
        if (staging_error ||
            !std::filesystem::is_directory(staging_status) ||
            unsafe_cache_link(staging, staging_status))
            throw std::runtime_error(
                "Begrenztes Codegen-Cache-Staging ist kein sicherer Ordner.");
    }
    const auto temporary = staging / "artifact.tmp";
    const auto cleanup_staging = [&]() noexcept {
        std::error_code cleanup_error;
        const auto staging_status =
            std::filesystem::symlink_status(staging, cleanup_error);
        if (cleanup_error ||
            !std::filesystem::is_directory(staging_status) ||
            unsafe_cache_link(staging, staging_status))
            return;
        const auto temporary_status =
            std::filesystem::symlink_status(temporary, cleanup_error);
        if (!cleanup_error &&
            std::filesystem::is_regular_file(temporary_status) &&
            !unsafe_cache_link(temporary, temporary_status))
            static_cast<void>(
                std::filesystem::remove(temporary, cleanup_error));
        cleanup_error.clear();
        static_cast<void>(
            std::filesystem::remove(staging, cleanup_error));
    };
    try {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error(
                "Begrenztes Codegen-Cache-Artefakt konnte nicht "
                "geoeffnet werden.");
        output.write(content.data(),
                     static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output)
            throw std::runtime_error(
                "Begrenztes Codegen-Cache-Artefakt konnte nicht "
                "geschrieben werden.");
        std::error_code publish_error;
        if (!safe_cache_directory_chain(root_, path.parent_path()))
            throw std::runtime_error(
                "Begrenzter Codegen-Cachepfad wurde vor dem Publish unsicher.");
        std::filesystem::create_hard_link(
            temporary, path, publish_error);
        if (publish_error) {
            const auto concurrent =
                load_bounded(key, artifact_name, maximum_bytes);
            if (!concurrent || *concurrent != content)
                throw std::runtime_error(
                    "Begrenzter Codegen-Cache-Publish kollidiert mit "
                    "einem unsicheren oder abweichenden Artefakt.");
        }
        cleanup_staging();
    } catch (...) {
        cleanup_staging();
        throw;
    }
}

void CodegenCache::store_integrity_bounded(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::string_view content,
    const std::size_t maximum_payload_bytes) {
    const auto maximum_artifact_bytes =
        integrity_artifact_budget(maximum_payload_bytes);
    if (content.size() > maximum_payload_bytes)
        throw std::invalid_argument(
            "Integritaetsgebundener Cache ueberschreitet sein "
            "Payloadbudget.");
    const auto envelope =
        integrity_artifact_envelope(key, artifact_name, content);
    const auto current =
        load_bounded(
            key, artifact_name, maximum_artifact_bytes);
    if (current) {
        const auto parsed =
            parse_integrity_artifact(
                key,
                artifact_name,
                *current,
                maximum_payload_bytes);
        if (parsed) {
            if (*parsed == content) return;
            throw std::runtime_error(
                "Integritaetsgebundener Cache-Schluessel kollidiert "
                "mit abweichendem Payload.");
        }
        if (!erase_bounded_if_matches(
                key,
                artifact_name,
                *current,
                maximum_artifact_bytes)) {
            const auto concurrent =
                load_integrity_bounded(
                    key,
                    artifact_name,
                    maximum_payload_bytes);
            if (concurrent && *concurrent == content)
                return;
            throw std::runtime_error(
                "Integritaetsgebundener Cache konnte ein ungueltiges "
                "Artefakt nicht sicher reparieren.");
        }
    }
    store_bounded(
        key,
        artifact_name,
        envelope,
        maximum_artifact_bytes);
}

bool CodegenCache::erase_bounded_if_matches(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::string_view expected_content,
    const std::size_t maximum_bytes) {
    if (maximum_bytes == 0u ||
        expected_content.size() > maximum_bytes)
        throw std::invalid_argument(
            "Begrenzte Codegen-Cache-Reparatur besitzt ein "
            "ungueltiges Bytebudget.");
    const auto current =
        load_bounded(key, artifact_name, maximum_bytes);
    if (!current || *current != expected_content) return false;
    const auto path = artifact_path(key, artifact_name);
    if (!safe_cache_directory_chain(root_, path.parent_path()))
        return false;
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_cache_link(path, status))
        return false;
    const auto removed = std::filesystem::remove(path, status_error);
    return removed && !status_error;
}

void CodegenCache::store(const std::string_view key,
                         const std::string_view artifact_name,
                         const std::string_view content) {
    store_bounded(
        key,
        artifact_name,
        content,
        maximum_legacy_codegen_cache_artifact_bytes);
}

const std::filesystem::path& CodegenCache::root() const noexcept {
    return root_;
}

std::filesystem::path CodegenCache::artifact_path(const std::string_view key,
                                                  const std::string_view artifact_name) const {
    const std::filesystem::path relative(artifact_name);
    if (!safe_component(key) || relative.empty() || relative.is_absolute()) {
        throw std::invalid_argument("Codegen-Cache-Pfadkomponente ist nicht portabel.");
    }
    for (const auto& component : relative) {
        if (!safe_component(component.string())) {
            throw std::invalid_argument("Codegen-Cache-Pfadkomponente ist nicht portabel.");
        }
    }
    const auto direct = root_ / std::string(key) / relative;
    if (direct.native().size() <= maximum_portable_cache_path_characters)
        return direct;

    // Logical identities remain complete. Only their local physical layout is
    // collapsed to one SHA-256 component over the unambiguous key/artifact
    // pair, retaining the full collision resistance without a long path.
    std::ostringstream compact_identity;
    append_field(compact_identity, key);
    append_field(compact_identity, relative.generic_string());
    return root_ / ".compact" /
           katana::io::sha256_bytes(compact_identity.str());
}

} // namespace katana::codegen
