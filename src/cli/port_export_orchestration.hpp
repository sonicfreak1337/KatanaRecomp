#pragma once

#include "katana/codegen/partition.hpp"
#include "katana/codegen/latent_aot_registry.hpp"
#include "katana/codegen/native_disc_analysis_artifact.hpp"
#include "katana/runtime/disc_install.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace katana::cli {

inline constexpr std::uint32_t port_export_cache_version = 11u;
inline constexpr std::uint32_t port_ir_contract_version = 3u;

struct PortExportAnalysisGenerationCacheBinding final {
    std::string_view artifact_identity;
    std::string_view archive_sha256;
    std::string_view committed_generation_identity;
};

struct PortExportImplementationIdentities final {
    std::string analysis;
    std::string analysis_cache;
    std::string ir_product;
    std::string codegen;
    std::string whole_export;
};

// Compact, in-memory authority retained while a refreshed analysis generation
// is evaluated.  It is deliberately not an artifact format: exact module
// identity and the old per-function instruction membership are only needed to
// distinguish a sound latent owner refinement from a lost graph edge.
struct AgentLatentFunctionAuthority final {
    std::uint32_t entry_address = 0u;
    std::vector<std::uint32_t> instruction_addresses;
    struct Edge final {
        std::uint32_t source_address = 0u;
        std::uint32_t target_address = 0u;

        [[nodiscard]] bool operator==(const Edge&) const = default;
        [[nodiscard]] bool operator<(const Edge& other) const noexcept {
            return source_address < other.source_address ||
                   (source_address == other.source_address &&
                    target_address < other.target_address);
        }
    };
    std::vector<Edge> edges;
};

struct AgentLatentModuleAuthority final {
    std::string id;
    std::string byte_identity;
    std::uint32_t byte_size = 0u;
    std::uint32_t source_address = 0u;
    std::vector<katana::codegen::PreparedLatentAotSourceBinding>
        source_bindings;
    std::vector<std::uint32_t> entry_offsets;
    std::vector<std::uint32_t> instruction_addresses;
    std::vector<AgentLatentFunctionAuthority> functions;
};

[[nodiscard]] bool agent_program_index_incoming_authority_preserved(
    const std::vector<katana::codegen::NativeDiscProgramIndexAdjacency>&
        required_incoming,
    const std::vector<katana::codegen::NativeDiscProgramIndexAdjacency>&
        candidate_incoming,
    const std::vector<katana::codegen::NativeDiscProgramIndexAdjacency>&
        required_outgoing,
    const std::vector<katana::codegen::NativeDiscProgramIndexAdjacency>&
        candidate_outgoing,
    const std::vector<AgentLatentModuleAuthority>& baseline_modules,
    const std::vector<AgentLatentModuleAuthority>& candidate_modules,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>&
        exact_entry_remaps = {});

[[nodiscard]] bool agent_latent_outgoing_owner_refinement_preserved(
    const std::vector<katana::codegen::NativeDiscProgramIndexAdjacency>&
        candidate_outgoing,
    std::uint32_t old_owner,
    std::uint32_t target,
    const std::vector<AgentLatentModuleAuthority>& baseline_modules,
    const std::vector<AgentLatentModuleAuthority>& candidate_modules);

[[nodiscard]] std::string port_export_recipe_identity(
    const katana::runtime::DiscInstallRecipe& recipe);

[[nodiscard]] std::string port_export_workspace_key(
    std::string_view source_kind,
    const katana::runtime::DiscInstallRecipe& recipe,
    std::string_view target_name);

[[nodiscard]] PortExportImplementationIdentities
port_export_implementation_identities(
    std::string_view native_port_artifact_identity = {},
    std::uint32_t native_port_artifact_format_version = 0u);

[[nodiscard]] std::string port_export_cache_key(
    std::string_view source_kind,
    std::uint32_t source_contract_version,
    const katana::runtime::DiscInstallRecipe& recipe,
    std::string_view boot_file_name,
    std::uint32_t entry_address,
    std::string_view target_name,
    bool diagnostic_partial,
    std::string_view console_profile,
    std::string_view game_project_identity,
    std::string_view game_entry_handoff_artifact_identity,
    std::string_view native_port_artifact_identity,
    std::uint32_t native_port_artifact_format_version,
    std::string_view latent_aot_entry_hint_identity,
    std::string_view analysis_mode_identity,
    std::string_view implementation_identity,
    const PortExportAnalysisGenerationCacheBinding& analysis_generation = {},
    const katana::codegen::PartitionOptions& partition_options = {});

} // namespace katana::cli
