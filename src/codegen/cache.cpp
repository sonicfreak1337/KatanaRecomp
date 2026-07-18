#include "katana/codegen/cache.hpp"

#include "katana/io/input_provenance.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace katana::codegen {
namespace {

void append_field(std::ostringstream& output, const std::string_view value) {
    output << value.size() << ':' << value << ';';
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
    if (const auto existing = load(key, artifact_name); existing) {
        if (*existing == content) return;
        throw std::runtime_error("Codegen-Cache-Schluessel kollidiert mit abweichendem Inhalt.");
    }
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::path staging;
    std::random_device random;
    for (std::size_t attempt = 0u; attempt < 32u; ++attempt) {
        staging = path.parent_path() /
                  (".publish-" + std::to_string(random()) + '-' + std::to_string(random()));
        std::error_code create_error;
        if (std::filesystem::create_directory(staging, create_error)) break;
        staging.clear();
    }
    if (staging.empty()) {
        throw std::runtime_error("Codegen-Cache-Staging konnte nicht atomar angelegt werden.");
    }
    const auto temporary = staging / "artifact.tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Codegen-Cache-Artefakt konnte nicht geoeffnet werden.");
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Codegen-Cache-Artefakt konnte nicht geschrieben werden.");
        }
        std::error_code publish_error;
        std::filesystem::create_hard_link(temporary, path, publish_error);
        if (publish_error) {
            const auto concurrent = load(key, artifact_name);
            if (!concurrent || *concurrent != content) {
                throw std::runtime_error(
                    "Codegen-Cache-Publish kollidiert mit abweichendem Artefakt.");
            }
        }
        std::filesystem::remove_all(staging);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging, cleanup_error);
        throw;
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
