#include "katana/codegen/cache.hpp"

#include "cache_secure_io.hpp" // Descriptor/handle-based bounded-cache I/O.

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::codegen {
namespace {

constexpr std::size_t maximum_portable_cache_path_characters = 220u;
constexpr std::size_t maximum_legacy_codegen_cache_artifact_bytes =
    256u * 1024u * 1024u;
constexpr std::size_t maximum_process_lru_entries = 262144u;
constexpr std::string_view integrity_artifact_magic{
    "KATANA-CACHE-ARTIFACT-V2\n"};
constexpr std::string_view integrity_artifact_family_magic{
    "KATANA-CACHE-ARTIFACT-V"};
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

struct IntegrityArtifactParse {
    CodegenCacheLoadState state = CodegenCacheLoadState::Corrupt;
    std::string content;
};

bool recognized_stale_integrity_artifact(
    const std::string_view artifact) noexcept {
    if (!artifact.starts_with(integrity_artifact_family_magic))
        return false;
    const auto version_begin = integrity_artifact_family_magic.size();
    const auto newline = artifact.find('\n', version_begin);
    if (newline == std::string_view::npos ||
        newline == version_begin)
        return false;
    for (auto index = version_begin; index < newline; ++index) {
        if (artifact[index] < '0' || artifact[index] > '9')
            return false;
    }
    return artifact.substr(version_begin, newline - version_begin) != "2";
}

IntegrityArtifactParse parse_integrity_artifact(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::string_view artifact,
    const std::size_t maximum_payload_bytes) {
    if (!artifact.starts_with(integrity_artifact_magic))
        return {
            recognized_stale_integrity_artifact(artifact)
                ? CodegenCacheLoadState::Stale
                : CodegenCacheLoadState::Corrupt,
            {}};
    if (artifact.size() < integrity_artifact_overhead)
        return {};
    auto cursor = integrity_artifact_magic.size();
    std::size_t payload_size = 0u;
    for (std::size_t index = 0u;
         index < integrity_artifact_size_digits;
         ++index) {
        const auto character = artifact[cursor + index];
        if (character < '0' || character > '9')
            return {};
        const auto digit =
            static_cast<std::size_t>(character - '0');
        if (payload_size >
            (std::numeric_limits<std::size_t>::max() - digit) /
                10u)
            return {};
        payload_size = payload_size * 10u + digit;
    }
    cursor += integrity_artifact_size_digits;
    if (artifact[cursor++] != '\n')
        return {};
    const auto stored_sha256 = artifact.substr(
        cursor, integrity_artifact_sha256_characters);
    cursor += integrity_artifact_sha256_characters;
    if (artifact[cursor++] != '\n' ||
        payload_size > maximum_payload_bytes ||
        payload_size != artifact.size() - cursor)
        return {};
    const auto payload = artifact.substr(cursor, payload_size);
    if (integrity_artifact_digest(key, artifact_name, payload) !=
        stored_sha256)
        return {};
    return {CodegenCacheLoadState::Hit, std::string(payload)};
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

bool transient_cache_component(const std::string_view value) noexcept {
    return value.starts_with(".publish-bounded-") ||
           value.starts_with(".erase-bounded-");
}

struct RootArtifactRecord {
    std::filesystem::path path;
    std::uint64_t bytes = 0u;
    std::filesystem::file_time_type last_write_time{};
    std::uint64_t access_sequence = 0u;
    bool evictable = true;
};

struct RootAccounting {
    std::mutex mutex;
    bool initialized = false;
    std::uint64_t total_bytes = 0u;
    std::uint64_t next_access_sequence = 0u;
    std::optional<std::uint64_t> observed_mutation_sequence;
    std::unordered_map<std::string, std::uint64_t> access_sequences;
    std::unordered_map<std::string, RootArtifactRecord> artifacts;
};

void invalidate_root_accounting(RootAccounting& accounting) noexcept {
    accounting.initialized = false;
    accounting.total_bytes = 0u;
    accounting.observed_mutation_sequence.reset();
    accounting.artifacts.clear();
    accounting.access_sequences.clear();
}

std::shared_ptr<RootAccounting> root_accounting(
    const std::filesystem::path& root) {
    static std::mutex registry_mutex;
    static std::unordered_map<
        std::string,
        std::shared_ptr<RootAccounting>> registry;
    const auto key = root.generic_string();
    const std::scoped_lock lock(registry_mutex);
    auto& accounting = registry[key];
    if (!accounting)
        accounting = std::make_shared<RootAccounting>();
    return accounting;
}

std::string root_artifact_identity(
    const std::filesystem::path& path) {
    return path.generic_string();
}

bool safe_root_relative_components(
    const std::filesystem::path& relative,
    bool& transient) {
    transient = false;
    if (relative.empty() || relative.is_absolute()) return false;
    std::size_t component_count = 0u;
    for (const auto& component : relative) {
        const auto value = component.string();
        if (!safe_component(value)) return false;
        transient = transient || transient_cache_component(value);
        ++component_count;
    }
    return component_count != 0u;
}

bool safe_root_artifact_layout(
    const std::filesystem::path& relative,
    bool& evictable) {
    bool transient = false;
    if (!safe_root_relative_components(relative, transient))
        return false;
    std::vector<std::string> components;
    for (const auto& component : relative)
        components.push_back(component.string());
    if (components.size() < 2u) return false;
    if (components.front() == ".compact" &&
        components.size() != 2u)
        return false;
    evictable = !transient;
    return true;
}

void scan_root_accounting(
    RootAccounting& accounting,
    const std::filesystem::path& root,
    const CodegenCacheRootLimits& limits) {
    std::error_code error;
    const auto root_status =
        std::filesystem::symlink_status(root, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            accounting.artifacts.clear();
            accounting.total_bytes = 0u;
            accounting.initialized = true;
            return;
        }
        throw std::runtime_error(
            "Codegen-Cache-Root konnte nicht sicher inventarisiert werden.");
    }
    if (root_status.type() == std::filesystem::file_type::not_found) {
        accounting.artifacts.clear();
        accounting.total_bytes = 0u;
        accounting.initialized = true;
        return;
    }
    if (!std::filesystem::is_directory(root_status) ||
        std::filesystem::is_symlink(root_status))
        throw std::runtime_error(
            "Codegen-Cache-Root ist kein sicheres Verzeichnis.");
#ifdef _WIN32
    const auto root_attributes = GetFileAttributesW(root.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        throw std::runtime_error(
            "Codegen-Cache-Root ist ein unsicherer Reparse-Punkt.");
#endif

    std::unordered_map<std::string, RootArtifactRecord> artifacts;
    std::uint64_t total_bytes = 0u;
    std::size_t scanned_entries = 0u;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error)
        throw std::runtime_error(
            "Codegen-Cache-Root konnte nicht sicher geoeffnet werden.");
    while (iterator != end) {
        if (++scanned_entries > limits.maximum_scan_entries)
            throw std::runtime_error(
                "Codegen-Cache-Root ueberschreitet sein Inventarbudget.");
        const auto path = iterator->path().lexically_normal();
        if (!detail::cache_path_within(root, path))
            throw std::runtime_error(
                "Codegen-Cache-Inventar verliess seinen Root.");
        const auto relative = path.lexically_relative(root);
        bool transient = false;
        if (!safe_root_relative_components(relative, transient))
            throw std::runtime_error(
                "Codegen-Cache-Inventar enthaelt einen unsicheren Pfad.");
        const auto status = iterator->symlink_status(error);
        if (error || std::filesystem::is_symlink(status))
            throw std::runtime_error(
                "Codegen-Cache-Inventar enthaelt einen unsicheren Link.");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            throw std::runtime_error(
                "Codegen-Cache-Inventar enthaelt einen unsicheren "
                "Reparse-Punkt.");
#endif
        if (std::filesystem::is_directory(status)) {
            iterator.increment(error);
            if (error)
                throw std::runtime_error(
                    "Codegen-Cache-Inventar konnte nicht vollstaendig gelesen werden.");
            continue;
        }
        if (!std::filesystem::is_regular_file(status))
            throw std::runtime_error(
                "Codegen-Cache-Inventar enthaelt einen unsicheren Dateityp.");
        if (relative == std::filesystem::path(
                            detail::secure_cache_root_lock_artifact_name)) {
            iterator.increment(error);
            if (error)
                throw std::runtime_error(
                    "Codegen-Cache-Inventar konnte nicht vollstaendig gelesen werden.");
            continue;
        }
        bool evictable = false;
        if (!safe_root_artifact_layout(relative, evictable))
            throw std::runtime_error(
                "Codegen-Cache-Inventar enthaelt ein fremdes Artefakt.");
        const auto raw_size = std::filesystem::file_size(path, error);
        if (error || raw_size >
                         std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error(
                "Codegen-Cache-Artefaktgroesse ist nicht darstellbar.");
        const auto bytes = static_cast<std::uint64_t>(raw_size);
        if (total_bytes >
            std::numeric_limits<std::uint64_t>::max() - bytes)
            throw std::runtime_error(
                "Codegen-Cache-Rootgroesse ist nicht darstellbar.");
        const auto write_time =
            std::filesystem::last_write_time(path, error);
        if (error)
            throw std::runtime_error(
                "Codegen-Cache-LRU-Zeit konnte nicht gelesen werden.");
        const auto identity = root_artifact_identity(path);
        const auto access = accounting.access_sequences.find(identity);
        RootArtifactRecord record;
        record.path = path;
        record.bytes = bytes;
        record.last_write_time = write_time;
        record.access_sequence =
            access == accounting.access_sequences.end()
                ? 0u
                : access->second;
        record.evictable = evictable;
        if (!artifacts.emplace(identity, std::move(record)).second)
            throw std::runtime_error(
                "Codegen-Cache-Inventar enthaelt eine doppelte Identitaet.");
        total_bytes += bytes;

        iterator.increment(error);
        if (error)
            throw std::runtime_error(
                "Codegen-Cache-Inventar konnte nicht vollstaendig gelesen werden.");
    }
    accounting.artifacts = std::move(artifacts);
    accounting.total_bytes = total_bytes;
    accounting.initialized = true;
    for (auto access = accounting.access_sequences.begin();
         access != accounting.access_sequences.end();) {
        if (!accounting.artifacts.contains(access->first))
            access = accounting.access_sequences.erase(access);
        else
            ++access;
    }
}

void synchronize_root_accounting(
    RootAccounting& accounting,
    const std::filesystem::path& root,
    const CodegenCacheRootLimits& limits,
    const std::optional<std::uint64_t> mutation_sequence) {
    if (!accounting.initialized ||
        !mutation_sequence.has_value() ||
        accounting.observed_mutation_sequence != mutation_sequence)
        scan_root_accounting(accounting, root, limits);
    accounting.observed_mutation_sequence = mutation_sequence;
}

void record_cache_access_locked(
    RootAccounting& accounting,
    const std::filesystem::path& path) {
    if (accounting.next_access_sequence ==
        std::numeric_limits<std::uint64_t>::max()) {
        accounting.next_access_sequence = 0u;
        accounting.access_sequences.clear();
        for (auto& [identity, artifact] : accounting.artifacts)
            artifact.access_sequence = 0u;
    }
    const auto identity = root_artifact_identity(path);
    if (!accounting.access_sequences.contains(identity) &&
        accounting.access_sequences.size() >=
            maximum_process_lru_entries)
        accounting.access_sequences.clear();
    const auto sequence = ++accounting.next_access_sequence;
    accounting.access_sequences[identity] = sequence;
    if (const auto artifact = accounting.artifacts.find(identity);
        artifact != accounting.artifacts.end())
        artifact->second.access_sequence = sequence;
}

void record_cache_access(
    const std::filesystem::path& root,
    const std::filesystem::path& path) {
    const auto accounting = root_accounting(root);
    const std::scoped_lock lock(accounting->mutex);
    record_cache_access_locked(*accounting, path);
}

bool root_budget_fits(
    const RootAccounting& accounting,
    const CodegenCacheRootLimits& limits,
    const std::uint64_t reserved_bytes,
    const std::size_t reserved_artifacts) noexcept {
    return reserved_bytes <= limits.maximum_bytes &&
           accounting.total_bytes <=
               limits.maximum_bytes - reserved_bytes &&
           reserved_artifacts <= limits.maximum_artifacts &&
           accounting.artifacts.size() <=
               limits.maximum_artifacts - reserved_artifacts;
}

void remove_accounted_artifact(
    RootAccounting& accounting,
    const std::string& identity) {
    const auto artifact = accounting.artifacts.find(identity);
    if (artifact == accounting.artifacts.end()) {
        accounting.access_sequences.erase(identity);
        return;
    }
    if (artifact->second.bytes > accounting.total_bytes)
        throw std::runtime_error(
            "Codegen-Cache-Rootbuchfuehrung ist inkonsistent.");
    accounting.total_bytes -= artifact->second.bytes;
    accounting.artifacts.erase(artifact);
    accounting.access_sequences.erase(identity);
}

bool older_root_artifact(
    const RootArtifactRecord& left,
    const RootArtifactRecord& right) {
    if ((left.access_sequence == 0u) !=
        (right.access_sequence == 0u))
        return left.access_sequence == 0u;
    if (left.access_sequence != right.access_sequence)
        return left.access_sequence < right.access_sequence;
    if (left.last_write_time != right.last_write_time)
        return left.last_write_time < right.last_write_time;
    return root_artifact_identity(left.path) <
           root_artifact_identity(right.path);
}

void enforce_root_budget(
    RootAccounting& accounting,
    const std::filesystem::path& root,
    const CodegenCacheRootLimits& limits,
    const std::filesystem::path& protected_path,
    const std::uint64_t reserved_bytes,
    const std::size_t reserved_artifacts) {
    if (!accounting.initialized)
        throw std::logic_error(
            "Codegen-Cache-Rootbuchfuehrung wurde nicht synchronisiert.");
    if (root_budget_fits(
            accounting, limits, reserved_bytes, reserved_artifacts))
        return;

    // Reconcile external writers only at the capacity boundary. Normal
    // partition publishing therefore remains O(N), not O(N^2).
    scan_root_accounting(accounting, root, limits);
    if (root_budget_fits(
            accounting, limits, reserved_bytes, reserved_artifacts))
        return;

    // Once a real capacity miss is confirmed, free a bounded burst of
    // headroom. Evicting only enough for the current artifact would make a
    // cold partition publish rescan the complete root before every write.
    // The requested reservation remains the lower bound, including for
    // limits too small to yield fractional headroom.
    const auto eviction_reserved_bytes = std::max(
        reserved_bytes, limits.maximum_bytes / 8u);
    const auto eviction_reserved_artifacts = std::max(
        reserved_artifacts, limits.maximum_artifacts / 8u);
    for (std::size_t reconciliation = 0u;
         reconciliation < 3u;
         ++reconciliation) {
        if (root_budget_fits(
                accounting,
                limits,
                eviction_reserved_bytes,
                eviction_reserved_artifacts))
            return;
        std::vector<std::string> candidates;
        candidates.reserve(accounting.artifacts.size());
        const auto protected_identity =
            root_artifact_identity(protected_path);
        for (const auto& [identity, artifact] :
             accounting.artifacts) {
            if (artifact.evictable &&
                identity != protected_identity)
                candidates.push_back(identity);
        }
        std::sort(
            candidates.begin(),
            candidates.end(),
            [&](const std::string& left,
                const std::string& right) {
                return older_root_artifact(
                    accounting.artifacts.at(left),
                    accounting.artifacts.at(right));
            });
        bool raced = false;
        for (const auto& identity : candidates) {
            const auto artifact = accounting.artifacts.find(identity);
            if (artifact == accounting.artifacts.end()) continue;
            const auto artifact_path = artifact->second.path;
            const auto read = detail::secure_cache_read(
                root,
                artifact_path,
                maximum_legacy_codegen_cache_artifact_bytes);
            bool erased = false;
            if (read.kind == detail::SecureArtifactKind::Missing) {
                erased = true;
            } else if (read.kind ==
                       detail::SecureArtifactKind::Regular) {
                erased = detail::secure_cache_erase_if_matches(
                    root,
                    artifact_path,
                    read.content,
                    maximum_legacy_codegen_cache_artifact_bytes);
            } else if (read.kind ==
                       detail::SecureArtifactKind::Oversized) {
                erased = detail::secure_cache_erase_oversized(
                    root,
                    artifact_path,
                    maximum_legacy_codegen_cache_artifact_bytes);
            } else {
                throw std::runtime_error(
                    "Codegen-Cache-LRU verweigert ein unsicheres Artefakt.");
            }
            if (!erased) {
                raced = true;
                break;
            }
            remove_accounted_artifact(accounting, identity);
            detail::secure_cache_prune_empty_parents(
                root, artifact_path.parent_path());
            if (root_budget_fits(
                    accounting,
                    limits,
                    eviction_reserved_bytes,
                    eviction_reserved_artifacts))
                return;
        }
        if (!raced)
            throw std::runtime_error(
                "Codegen-Cache-Rootbudget kann nicht sicher freigemacht werden.");
        scan_root_accounting(accounting, root, limits);
    }
    throw std::runtime_error(
        "Codegen-Cache-Rootbudget blieb durch konkurrierende Aenderungen instabil.");
}

void record_published_artifact(
    RootAccounting& accounting,
    const std::filesystem::path& path,
    const std::uint64_t bytes) {
    const auto identity = root_artifact_identity(path);
    remove_accounted_artifact(accounting, identity);
    if (accounting.total_bytes >
        std::numeric_limits<std::uint64_t>::max() - bytes)
        throw std::runtime_error(
            "Codegen-Cache-Rootgroesse ist nicht darstellbar.");
    if (accounting.next_access_sequence ==
        std::numeric_limits<std::uint64_t>::max()) {
        accounting.next_access_sequence = 0u;
        accounting.access_sequences.clear();
        for (auto& [artifact_identity, artifact] :
             accounting.artifacts)
            artifact.access_sequence = 0u;
    }
    const auto sequence = ++accounting.next_access_sequence;
    std::error_code error;
    auto write_time = std::filesystem::last_write_time(path, error);
    if (error)
        write_time = std::filesystem::file_time_type::clock::now();
    RootArtifactRecord record;
    record.path = path;
    record.bytes = bytes;
    record.last_write_time = write_time;
    record.access_sequence = sequence;
    accounting.artifacts.emplace(identity, std::move(record));
    accounting.access_sequences[identity] = sequence;
    accounting.total_bytes += bytes;
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

CodegenCache::CodegenCache(std::filesystem::path root)
    : CodegenCache(std::move(root), {}) {}

CodegenCache::CodegenCache(
    std::filesystem::path root,
    CodegenCacheRootLimits root_limits)
    : root_(std::move(root)),
      root_limits_(root_limits) {
    if (root_.empty()) {
        throw std::invalid_argument("Codegen-Cache braucht ein Stammverzeichnis.");
    }
    if (root_limits_.maximum_bytes == 0u ||
        root_limits_.maximum_artifacts == 0u ||
        root_limits_.maximum_scan_entries <
            root_limits_.maximum_artifacts)
        throw std::invalid_argument(
            "Codegen-Cache besitzt ein ungueltiges Rootbudget.");
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
    auto result = load_bounded_state(
        key, artifact_name, maximum_bytes);
    if (!result.hit()) return std::nullopt;
    return std::move(result.content);
}

CodegenCacheLoadResult CodegenCache::load_bounded_state(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::size_t maximum_bytes) const {
    if (maximum_bytes == 0u)
        throw std::invalid_argument(
            "Begrenzter Codegen-Cache-Read braucht ein Bytebudget.");
    const auto path = artifact_path(key, artifact_name);
    auto read = detail::secure_cache_read(
        root_, path, maximum_bytes);
    CodegenCacheLoadResult result;
    result.native_error = read.native_error;
    result.native_stage = read.native_stage;
    switch (read.kind) {
    case detail::SecureArtifactKind::Missing:
        result.state = CodegenCacheLoadState::Missing;
        break;
    case detail::SecureArtifactKind::Unsafe:
        result.state = CodegenCacheLoadState::Unsafe;
        break;
    case detail::SecureArtifactKind::Oversized:
        result.state = CodegenCacheLoadState::Oversized;
        break;
    case detail::SecureArtifactKind::Regular:
        result.state = CodegenCacheLoadState::Hit;
        result.content = std::move(read.content);
        try {
            record_cache_access(root_, path);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            // LRU bookkeeping is optional; the securely read payload remains
            // authoritative for this cache hit.
        }
        break;
    }
    return result;
}

std::optional<std::string>
CodegenCache::load_integrity_bounded(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::size_t maximum_payload_bytes) const {
    auto result = load_integrity_bounded_state(
        key, artifact_name, maximum_payload_bytes);
    if (!result.hit()) return std::nullopt;
    return std::move(result.content);
}

CodegenCacheLoadResult CodegenCache::load_integrity_bounded_state(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::size_t maximum_payload_bytes) const {
    const auto maximum_artifact_bytes =
        integrity_artifact_budget(maximum_payload_bytes);
    auto artifact = load_bounded_state(
        key, artifact_name, maximum_artifact_bytes);
    if (!artifact.hit()) return artifact;
    auto parsed = parse_integrity_artifact(
        key,
        artifact_name,
        artifact.content,
        maximum_payload_bytes);
    artifact.state = parsed.state;
    artifact.content = std::move(parsed.content);
    return artifact;
}

void CodegenCache::store_bounded(const std::string_view key,
                                 const std::string_view artifact_name,
                                 const std::string_view content,
                                 const std::size_t maximum_bytes) {
    if (maximum_bytes == 0u || content.size() > maximum_bytes ||
        static_cast<std::uint64_t>(content.size()) >
            root_limits_.maximum_bytes ||
        content.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max()))
        throw std::invalid_argument(
            "Begrenzter Codegen-Cache-Publish besitzt ein ungueltiges Bytebudget.");
    const auto path = artifact_path(key, artifact_name);
    const auto accounting = root_accounting(root_);
    const std::unique_lock accounting_lock(accounting->mutex);
    detail::SecureCacheRootMutationLock root_lock(root_);
    synchronize_root_accounting(
        *accounting,
        root_,
        root_limits_,
        root_lock.sequence());
    const auto existing = detail::secure_cache_read(
        root_, path, maximum_bytes);
    if (existing.kind == detail::SecureArtifactKind::Regular) {
        if (existing.content == content) {
            record_cache_access_locked(*accounting, path);
            return;
        }
        throw std::runtime_error(
            "Begrenzter Codegen-Cache-Schluessel kollidiert mit "
            "abweichendem Inhalt.");
    }
    if (existing.kind == detail::SecureArtifactKind::Unsafe)
        throw std::runtime_error(
            "Begrenzter Codegen-Cache verweigert ein unsicheres "
            "bestehendes Artefakt (key=" + std::string(key) +
            ", artifact=" + std::string(artifact_name) +
            ", native_error=" +
            std::to_string(existing.native_error) +
            ", native_stage=" +
            std::to_string(existing.native_stage) + ").");
    const auto mutation_sequence = root_lock.advance_sequence();
    try {
        if (existing.kind == detail::SecureArtifactKind::Oversized) {
            if (!detail::secure_cache_erase_oversized(
                    root_, path, maximum_bytes))
                throw std::runtime_error(
                    "Begrenzter Codegen-Cache verweigert ein unsicheres "
                    "bestehendes Artefakt.");
            remove_accounted_artifact(
                *accounting, root_artifact_identity(path));
        }
        enforce_root_budget(
            *accounting,
            root_,
            root_limits_,
            path,
            static_cast<std::uint64_t>(content.size()),
            1u);
        static_cast<void>(detail::secure_cache_publish(
            root_, path, content, maximum_bytes));
        record_published_artifact(
            *accounting,
            path,
            static_cast<std::uint64_t>(content.size()));
        accounting->observed_mutation_sequence = mutation_sequence;
    } catch (const std::bad_alloc&) {
        invalidate_root_accounting(*accounting);
        throw;
    } catch (...) {
        invalidate_root_accounting(*accounting);
        throw;
    }
}

bool CodegenCache::replace_bounded_if_matches(
    const std::string_view key,
    const std::string_view artifact_name,
    const std::string_view expected_content,
    const std::string_view replacement_content,
    const std::size_t maximum_bytes) {
    if (maximum_bytes == 0u ||
        expected_content.size() > maximum_bytes ||
        replacement_content.size() > maximum_bytes ||
        static_cast<std::uint64_t>(replacement_content.size()) >
            root_limits_.maximum_bytes ||
        replacement_content.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max()))
        throw std::invalid_argument(
            "Begrenzter Codegen-Cache-Replace besitzt ein ungueltiges "
            "Bytebudget.");
    const auto path = artifact_path(key, artifact_name);
    const auto accounting = root_accounting(root_);
    const std::unique_lock accounting_lock(accounting->mutex);
    detail::SecureCacheRootMutationLock root_lock(root_);
    synchronize_root_accounting(
        *accounting,
        root_,
        root_limits_,
        root_lock.sequence());
    const auto existing = detail::secure_cache_read(
        root_, path, maximum_bytes);
    if (existing.kind != detail::SecureArtifactKind::Regular ||
        existing.content != expected_content)
        return false;
    if (expected_content == replacement_content) {
        record_cache_access_locked(*accounting, path);
        return true;
    }
    const auto additional_bytes =
        replacement_content.size() > expected_content.size()
            ? static_cast<std::uint64_t>(
                  replacement_content.size() - expected_content.size())
            : 0u;
    enforce_root_budget(
        *accounting,
        root_,
        root_limits_,
        path,
        additional_bytes,
        0u);
    const auto mutation_sequence = root_lock.advance_sequence();
    bool replaced = false;
    try {
        replaced = detail::secure_cache_replace_if_matches(
            root_,
            path,
            expected_content,
            replacement_content,
            maximum_bytes);
    } catch (const std::bad_alloc&) {
        invalidate_root_accounting(*accounting);
        throw;
    } catch (...) {
        invalidate_root_accounting(*accounting);
        throw;
    }
    if (!replaced) {
        // A failed exact-CAS may have observed an uncooperative namespace
        // mutation at the platform commit boundary. The root sequence only
        // serializes cooperating cache writers, so do not bless the earlier
        // inventory snapshot as current.
        invalidate_root_accounting(*accounting);
        return false;
    }
    // The platform commit is the authority boundary. Accounting is only an
    // optimization; once replace/rename succeeded, an allocation or metadata
    // failure cannot truthfully turn the committed replacement into a failed
    // publish. Force a bounded rescan on the next operation instead.
    try {
        record_published_artifact(
            *accounting,
            path,
            static_cast<std::uint64_t>(replacement_content.size()));
        accounting->observed_mutation_sequence = mutation_sequence;
    } catch (...) {
        invalidate_root_accounting(*accounting);
    }
    return true;
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
        if (parsed.state == CodegenCacheLoadState::Hit) {
            if (parsed.content == content) return;
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
    const auto path = artifact_path(key, artifact_name);
    const auto accounting = root_accounting(root_);
    const std::unique_lock accounting_lock(accounting->mutex);
    detail::SecureCacheRootMutationLock root_lock(root_);
    synchronize_root_accounting(
        *accounting,
        root_,
        root_limits_,
        root_lock.sequence());
    const auto mutation_sequence = root_lock.advance_sequence();
    try {
        const auto erased = detail::secure_cache_erase_if_matches(
            root_,
            path,
            expected_content,
            maximum_bytes);
        if (erased) {
            remove_accounted_artifact(
                *accounting, root_artifact_identity(path));
            detail::secure_cache_prune_empty_parents(
                root_, path.parent_path());
        }
        accounting->observed_mutation_sequence = mutation_sequence;
        return erased;
    } catch (const std::bad_alloc&) {
        invalidate_root_accounting(*accounting);
        throw;
    } catch (...) {
        invalidate_root_accounting(*accounting);
        throw;
    }
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

const CodegenCacheRootLimits& CodegenCache::root_limits() const noexcept {
    return root_limits_;
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
