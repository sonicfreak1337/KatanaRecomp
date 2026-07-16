#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace katana::codegen {

inline constexpr std::uint32_t codegen_cache_schema_version = 2u;

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
};

[[nodiscard]] std::string make_codegen_cache_key(const CodegenCacheInputs& inputs);

class CodegenCache final {
  public:
    explicit CodegenCache(std::filesystem::path root);

    [[nodiscard]] std::optional<std::string> load(std::string_view key,
                                                  std::string_view artifact_name) const;
    void store(std::string_view key, std::string_view artifact_name, std::string_view content);
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

  private:
    [[nodiscard]] std::filesystem::path artifact_path(std::string_view key,
                                                      std::string_view artifact_name) const;

    std::filesystem::path root_;
};

} // namespace katana::codegen
