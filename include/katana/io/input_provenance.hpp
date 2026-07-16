#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace katana::io {

inline constexpr std::uint32_t input_provenance_schema_version = 1u;

struct InputProvenance {
    std::string role;
    std::uint64_t size = 0u;
    std::string sha256;
    std::filesystem::path local_path;
};

struct BuildProvenance {
    std::string tool_version;
    std::uint32_t manifest_version = 0u;
    std::string manifest_sha256;
    std::string directives_sha256;
    std::uint32_t ir_version = 0u;
    std::uint32_t runtime_abi = 0u;
    std::string backend_name;
    std::uint32_t backend_abi = 0u;
    std::vector<InputProvenance> inputs;
};

[[nodiscard]] std::string sha256_bytes(std::string_view bytes);
[[nodiscard]] InputProvenance capture_input_provenance(std::string role,
                                                       const std::filesystem::path& path);
[[nodiscard]] std::string make_portable_build_identity(const BuildProvenance& provenance);
[[nodiscard]] std::string format_build_provenance_json(const BuildProvenance& provenance);

} // namespace katana::io
