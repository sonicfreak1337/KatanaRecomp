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
inline constexpr std::uint32_t native_port_artifact_format_version = 15u;
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
    struct ProviderSemanticStorage final {
        std::uint32_t contract_version =
            native_port_provider_semantics_contract_version;
        std::uint32_t hook_guest_address = 0u;
        bool authoritative = true;
        std::string provider_symbol;
        std::string semantic_identity;
        std::string expected_owner_semantic_identity;
        std::string provider_implementation_identity;
        std::vector<std::string> guard_expressions;
        std::vector<std::string> guard_paths;
        std::vector<NativePortProviderGuard> guards;
        std::vector<std::string> effect_regions;
        std::vector<std::string> effect_register_names;
        std::vector<std::string> effect_resources;
        std::vector<std::string> effect_addresses;
        std::vector<std::string> effect_values;
        std::vector<std::string> effect_results;
        std::vector<std::string> effect_paths;
        std::vector<NativePortProviderEffect> effects;
        std::string result_target_expression;
        std::string result_error_expression;
        std::string result_cpu_state_expression;
        std::string result_title_state_expression;
        NativePortProviderResultProjection result;
    };

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
    std::vector<std::uint32_t> bootstrap_post_aot_roots_;
    std::vector<NativePortAotContinuationBinding>
        bootstrap_post_aot_continuations_;
    std::string bootstrap_symbol_;
    std::string bootstrap_post_cpu_state_identity_;
    std::vector<std::string> bootstrap_write_pre_identities_;
    std::vector<std::string> bootstrap_write_post_identities_;
    std::vector<NativePortBootstrapWriteBinding> bootstrap_writes_;
    std::string acceptance_milestone_id_;
    std::uint32_t acceptance_witness_hook_guest_address_ = 0u;
    std::vector<std::string> checkpoint_runtime_image_ids_;
    std::vector<std::string_view> checkpoint_runtime_image_id_views_;

    std::vector<std::string> image_ids_;
    std::vector<std::string> image_paths_;
    std::vector<std::string> image_byte_identities_;
    std::vector<NativePortImageBinding> images_;

    std::vector<std::string> hook_symbols_;
    std::vector<std::string> hook_code_identities_;
    std::vector<std::string> hook_provider_implementation_identities_;
    std::vector<std::string> hook_code_source_identities_;
    std::vector<NativePortHookBinding> hooks_;

    std::vector<NativePortHardwareResolution> hardware_resolutions_;
    NativePortFrameTimingBinding frame_timing_;
    std::vector<ProviderSemanticStorage> provider_semantic_storage_;
    std::vector<NativePortProviderSemanticContract>
        provider_semantic_contract_views_;
    NativePortProviderSemanticCoverage provider_semantic_coverage_ =
        NativePortProviderSemanticCoverage::DeclaredOnly;
    NativePortInputOwnership input_ownership_ =
        NativePortInputOwnership::MapleDevice;

    NativePortDefinition definition_;
};

} // namespace katana::runtime
