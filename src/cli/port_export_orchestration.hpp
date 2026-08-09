#pragma once

#include "katana/codegen/partition.hpp"
#include "katana/runtime/disc_install.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace katana::cli {

inline constexpr std::uint32_t port_export_cache_version = 9u;
inline constexpr std::uint32_t port_ir_contract_version = 2u;

struct PortExportImplementationIdentities final {
    std::string analysis;
    std::string analysis_cache;
    std::string codegen;
    std::string whole_export;
};

[[nodiscard]] std::string port_export_recipe_identity(
    const katana::runtime::DiscInstallRecipe& recipe);

[[nodiscard]] std::string port_export_workspace_key(
    std::string_view source_kind,
    const katana::runtime::DiscInstallRecipe& recipe,
    std::string_view target_name);

[[nodiscard]] PortExportImplementationIdentities
port_export_implementation_identities();

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
    std::string_view latent_aot_entry_hint_identity,
    std::string_view analysis_mode_identity,
    std::string_view implementation_identity,
    const katana::codegen::PartitionOptions& partition_options = {});

} // namespace katana::cli
