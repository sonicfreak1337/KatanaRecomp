#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace katana::codegen {

inline constexpr std::uint32_t codegen_cache_schema_version = 5u;

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

    [[nodiscard]] std::optional<std::string> load(std::string_view key,
                                                  std::string_view artifact_name) const;
    // Reads only regular, non-symlink artifacts whose exact size fits the
    // caller's validation budget. Oversized or structurally unsafe entries are
    // cache misses; an artifact is never partially returned.
    [[nodiscard]] std::optional<std::string>
    load_bounded(std::string_view key,
                 std::string_view artifact_name,
                 std::size_t maximum_bytes) const;
    // Integrity-bound variant for artifacts whose payload is consumed as
    // executable source or authoritative metadata. A checksum-invalid,
    // truncated, foreign or legacy raw artifact is a cache miss.
    [[nodiscard]] std::optional<std::string>
    load_integrity_bounded(std::string_view key,
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

  private:
    [[nodiscard]] std::filesystem::path artifact_path(std::string_view key,
                                                      std::string_view artifact_name) const;

    std::filesystem::path root_;
};

} // namespace katana::codegen
