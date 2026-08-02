#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace katana::codegen {

inline constexpr std::uint32_t codegen_cache_schema_version = 5u;

enum class CodegenCacheLoadState : std::uint8_t {
    Missing,
    Hit,
    Stale,
    Corrupt,
    Oversized,
    Unsafe,
};

struct CodegenCacheLoadResult {
    CodegenCacheLoadState state = CodegenCacheLoadState::Missing;
    std::string content;
    std::uint32_t native_error = 0u;
    std::uint32_t native_stage = 0u;

    [[nodiscard]] bool hit() const noexcept {
        return state == CodegenCacheLoadState::Hit;
    }
};

struct CodegenCacheRootLimits {
    std::uint64_t maximum_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    std::size_t maximum_artifacts = 65536u;
    std::size_t maximum_scan_entries = 262144u;
};

struct CodegenCacheInputs {
    std::string input_hash;
    std::string ir_hash;
    std::string configuration_hash;
    std::string backend_name;
    std::uint32_t backend_abi = 0u;
    std::uint32_t runtime_abi = 0u;
    std::string manifest_hash;
    std::string overrides_hash;
    std::uint32_t ir_version = 0u;
    std::uint32_t optimization_version = 0u;
    std::string tool_version;
    // Exact identity of the implementation which serialized the cached
    // artifact. ABI and marketing versions alone cannot distinguish two
    // dirty/local exporter binaries with different emission logic.
    std::string implementation_identity;
};

[[nodiscard]] std::string make_codegen_cache_key(const CodegenCacheInputs& inputs);

class CodegenCache final {
  public:
    explicit CodegenCache(std::filesystem::path root);
    CodegenCache(std::filesystem::path root,
                 CodegenCacheRootLimits root_limits);

    [[nodiscard]] std::optional<std::string> load(std::string_view key,
                                                  std::string_view artifact_name) const;
    // Reads only regular, non-symlink artifacts whose exact size fits the
    // caller's validation budget. Oversized or structurally unsafe entries are
    // cache misses; an artifact is never partially returned.
    [[nodiscard]] std::optional<std::string>
    load_bounded(std::string_view key,
                 std::string_view artifact_name,
                 std::size_t maximum_bytes) const;
    // Diagnostic form of load_bounded(). Missing, oversized and structurally
    // unsafe entries remain distinct without weakening the bounded reader.
    [[nodiscard]] CodegenCacheLoadResult
    load_bounded_state(std::string_view key,
                       std::string_view artifact_name,
                       std::size_t maximum_bytes) const;
    // Integrity-bound variant for artifacts whose payload is consumed as
    // executable source or authoritative metadata. A checksum-invalid,
    // truncated, foreign or legacy raw artifact is a cache miss.
    [[nodiscard]] std::optional<std::string>
    load_integrity_bounded(std::string_view key,
                           std::string_view artifact_name,
                           std::size_t maximum_payload_bytes) const;
    // Also distinguishes a recognized older envelope from malformed/current
    // content. Callers may use Stale for their own schema validation results.
    [[nodiscard]] CodegenCacheLoadResult
    load_integrity_bounded_state(
        std::string_view key,
        std::string_view artifact_name,
        std::size_t maximum_payload_bytes) const;
    // Publishes without ever falling back to the unbounded reader. Existing
    // unsafe entries are rejected; an oversized regular artifact is removed
    // without reading it, and a concurrent byte-identical bounded publisher
    // is accepted.
    void store_bounded(std::string_view key,
                       std::string_view artifact_name,
                       std::string_view content,
                       std::size_t maximum_bytes);
    // Publishes a canonical size- and SHA-256-bound envelope. A previously
    // observed malformed regular artifact is replaced only through the same
    // exact-content bounded erase contract used for cache repair.
    void store_integrity_bounded(std::string_view key,
                                 std::string_view artifact_name,
                                 std::string_view content,
                                 std::size_t maximum_payload_bytes);
    // Removes only the exact regular, non-symlink artifact previously read
    // within the same bound. It never recursively removes a cache path.
    [[nodiscard]] bool
    erase_bounded_if_matches(std::string_view key,
                             std::string_view artifact_name,
                             std::string_view expected_content,
                             std::size_t maximum_bytes);
    void store(std::string_view key, std::string_view artifact_name, std::string_view content);
    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] const CodegenCacheRootLimits& root_limits() const noexcept;

  private:
    [[nodiscard]] std::filesystem::path artifact_path(std::string_view key,
                                                      std::string_view artifact_name) const;

    std::filesystem::path root_;
    CodegenCacheRootLimits root_limits_;
};

} // namespace katana::codegen
