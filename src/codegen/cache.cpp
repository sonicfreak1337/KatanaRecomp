#include "katana/codegen/cache.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace katana::codegen {
namespace {

void hash_bytes(std::uint64_t& hash, const std::string_view value) noexcept {
    for (const auto byte : value) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
}

void hash_field(std::uint64_t& hash, const std::string_view value) noexcept {
    hash_bytes(hash, std::to_string(value.size()));
    hash_bytes(hash, ":");
    hash_bytes(hash, value);
    hash_bytes(hash, ";");
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
        inputs.optimization_version == 0u || inputs.tool_version.empty()) {
        throw std::invalid_argument("Codegen-Cache-Schluessel ist unvollstaendig.");
    }
    std::uint64_t hash = 14695981039346656037ull;
    hash_field(hash, std::to_string(codegen_cache_schema_version));
    hash_field(hash, inputs.input_hash);
    hash_field(hash, inputs.ir_hash);
    hash_field(hash, inputs.configuration_hash);
    hash_field(hash, inputs.backend_name);
    hash_field(hash, std::to_string(inputs.backend_abi));
    hash_field(hash, std::to_string(inputs.runtime_abi));
    hash_field(hash, inputs.manifest_hash);
    hash_field(hash, inputs.overrides_hash);
    hash_field(hash, std::to_string(inputs.ir_version));
    hash_field(hash, std::to_string(inputs.optimization_version));
    hash_field(hash, inputs.tool_version);
    std::ostringstream output;
    output << "cg-v" << codegen_cache_schema_version << '-' << std::hex << std::setfill('0')
           << std::setw(16) << hash;
    return output.str();
}

CodegenCache::CodegenCache(std::filesystem::path root) : root_(std::move(root)) {
    if (root_.empty()) {
        throw std::invalid_argument("Codegen-Cache braucht ein Stammverzeichnis.");
    }
    root_ = std::filesystem::absolute(root_).lexically_normal();
}

std::optional<std::string> CodegenCache::load(const std::string_view key,
                                              const std::string_view artifact_name) const {
    const auto path = artifact_path(key, artifact_name);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (!input.eof() && input.fail()) {
        throw std::runtime_error("Codegen-Cache-Artefakt konnte nicht gelesen werden.");
    }
    return content.str();
}

void CodegenCache::store(const std::string_view key,
                         const std::string_view artifact_name,
                         const std::string_view content) {
    const auto path = artifact_path(key, artifact_name);
    if (const auto existing = load(key, artifact_name); existing && *existing == content) {
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Codegen-Cache-Artefakt konnte nicht geoeffnet werden.");
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error("Codegen-Cache-Artefakt konnte nicht geschrieben werden.");
    }
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
    return root_ / std::string(key) / relative;
}

} // namespace katana::codegen
