#pragma once

#include "katana/runtime/native_port.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace katana::runtime {

// This private tooling artifact has an independent wire contract.  It owns
// only static NativePortDefinition data; retail image bytes and every process
// local hook/bootstrap callback remain outside the artifact.
inline constexpr std::uint32_t native_port_artifact_format_version = 1u;
inline constexpr std::uint64_t native_port_artifact_maximum_size =
    16u * 1024u * 1024u;

class NativePortArtifact final {
  public:
    NativePortArtifact(const NativePortArtifact&) = delete;
    NativePortArtifact& operator=(const NativePortArtifact&) = delete;
    NativePortArtifact(NativePortArtifact&&) = delete;
    NativePortArtifact& operator=(NativePortArtifact&&) = delete;
    ~NativePortArtifact() = default;

    [[nodiscard]] static std::shared_ptr<NativePortArtifact>
    load(const std::filesystem::path& path);

    // Validates before serialization and again after the owned artifact has
    // been reconstructed.  The return value always describes the atomically
    // published, canonical non-symlink file.
    [[nodiscard]] static std::shared_ptr<NativePortArtifact>
    write(const std::filesystem::path& path,
          const NativePortDefinition& definition);

    [[nodiscard]] const std::filesystem::path&
    canonical_path() const noexcept;
    [[nodiscard]] const std::string& artifact_identity() const noexcept;
    [[nodiscard]] const NativePortDefinition& definition() const noexcept;

  private:
    NativePortArtifact() = default;

    void rebuild_definition();

    std::filesystem::path canonical_path_;
    std::string artifact_identity_;

    std::uint32_t contract_version_ =
        native_port_definition_contract_version;
    std::string project_id_;
    std::string project_version_;
    std::string executable_content_identity_;
    std::string executable_name_;
    std::string executable_byte_identity_;
    NativePortBootstrap bootstrap_;
    std::string bootstrap_symbol_;

    std::vector<std::string> image_ids_;
    std::vector<std::string> image_paths_;
    std::vector<std::string> image_byte_identities_;
    std::vector<NativePortImageBinding> images_;

    std::vector<std::string> hook_symbols_;
    std::vector<std::string> hook_code_identities_;
    std::vector<NativePortHookBinding> hooks_;

    std::vector<std::string> hardware_resolution_image_ids_;
    std::vector<NativePortHardwareResolution> hardware_resolutions_;

    NativePortDefinition definition_;
};

} // namespace katana::runtime
