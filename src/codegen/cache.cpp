#include "katana/codegen/cache.hpp"

#include "cache_secure_io.hpp" // Descriptor/handle-based bounded-cache I/O.

#include "katana/io/input_provenance.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

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
    const auto read = detail::secure_cache_read(
        root_, artifact_path(key, artifact_name), maximum_bytes);
    if (read.kind != detail::SecureArtifactKind::Regular)
        return std::nullopt;
    return read.content;
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
    const auto existing =
        detail::secure_cache_read(root_, path, maximum_bytes);
    if (existing.kind == detail::SecureArtifactKind::Regular) {
        if (existing.content == content) return;
        throw std::runtime_error(
            "Begrenzter Codegen-Cache-Schluessel kollidiert mit "
            "abweichendem Inhalt.");
    }
    if (existing.kind == detail::SecureArtifactKind::Unsafe)
        throw std::runtime_error(
            "Begrenzter Codegen-Cache verweigert ein unsicheres "
            "bestehendes Artefakt.");
    if (existing.kind == detail::SecureArtifactKind::Oversized) {
        if (!detail::secure_cache_erase_oversized(
                root_, path, maximum_bytes))
            throw std::runtime_error(
                "Begrenzter Codegen-Cache verweigert ein unsicheres "
                "bestehendes Artefakt.");
    }
    detail::secure_cache_publish(
        root_, path, content, maximum_bytes);
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
    return detail::secure_cache_erase_if_matches(
        root_,
        artifact_path(key, artifact_name),
        expected_content,
        maximum_bytes);
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
