#include "port_export_orchestration.hpp"

#include "katana/component_identity.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cache.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/codegen/port_export.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/runtime/abi.hpp"
#include "katana/runtime/game_project.hpp"
#include "katana/runtime/game_project_artifact.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <sstream>
#include <stdexcept>

namespace katana::cli {
namespace {

void append_cache_field(
    std::ostringstream& output,
    const std::string_view value) {
    output << value.size() << ':' << value << ';';
}

bool valid_cache_digest(const std::string_view value) noexcept {
    return value.size() == 64u &&
           std::all_of(
               value.begin(),
               value.end(),
               [](const unsigned char character) {
                   return std::isdigit(character) != 0 ||
                          (character >= 'a' &&
                           character <= 'f');
               });
}

} // namespace

std::string port_export_recipe_identity(
    const katana::runtime::DiscInstallRecipe& recipe) {
    return katana::io::sha256_bytes(
        katana::runtime::format_disc_install_recipe(recipe));
}

std::string port_export_workspace_key(
    const std::string_view source_kind,
    const katana::runtime::DiscInstallRecipe& recipe,
    const std::string_view target_name) {
    std::ostringstream identity;
    append_cache_field(identity, "katana-port-workspace");
    append_cache_field(identity, "1");
    append_cache_field(identity, source_kind);
    append_cache_field(identity, recipe.content_identity);
    append_cache_field(identity, recipe.boot_sha256);
    append_cache_field(identity, target_name);
    return katana::io::sha256_bytes(identity.str());
}

PortExportImplementationIdentities
port_export_implementation_identities() {
    if (!valid_cache_digest(
            katana::build_contract::analysis_component_identity) ||
        !valid_cache_digest(
            katana::build_contract::analysis_cache_component_identity) ||
        !valid_cache_digest(
            katana::build_contract::ir_component_identity) ||
        !valid_cache_digest(
            katana::build_contract::codegen_component_identity) ||
        !valid_cache_digest(
            katana::build_contract::orchestration_component_identity))
        throw std::runtime_error(
            "Exporter besitzt keine gueltigen buildgebundenen "
            "Komponentenidentitaeten.");

    const auto combine = [](const std::string_view kind,
                            const std::span<const std::string_view> fields) {
        std::ostringstream material;
        append_cache_field(material, kind);
        append_cache_field(material, "1");
        for (const auto field : fields)
            append_cache_field(material, field);
        return katana::io::sha256_bytes(material.str());
    };
    const auto analyzer_abi =
        std::to_string(katana::build_contract::analyzer_abi_version);
    const std::array<std::string_view, 2u> stable_analysis_fields{
        katana::build_contract::analysis_component_identity,
        analyzer_abi};
    PortExportImplementationIdentities identities;
    identities.analysis = combine(
        "katana-port-analysis-components",
        stable_analysis_fields);

    const auto ir_contract =
        std::to_string(port_ir_contract_version);
    const std::array<std::string_view, 4u> analysis_cache_fields{
        identities.analysis,
        katana::build_contract::analysis_cache_component_identity,
        katana::build_contract::ir_component_identity,
        ir_contract};
    identities.analysis_cache = combine(
        "katana-port-analysis-cache-components",
        analysis_cache_fields);

    const auto backend_abi =
        std::to_string(
            katana::codegen::backend_interface_abi_version);
    const auto partition_schema =
        std::to_string(
            katana::codegen::
                port_partition_emission_schema_version);
    const auto metadata_schema =
        std::to_string(
            katana::codegen::
                port_metadata_cache_schema_version);
    const auto codegen_schema =
        std::to_string(
            katana::codegen::codegen_cache_schema_version);
    const auto native_profile =
        std::to_string(
            katana::codegen::
                native_aot_emission_profile_version);
    const std::array<std::string_view, 6u> codegen_fields{
        katana::build_contract::codegen_component_identity,
        backend_abi,
        partition_schema,
        metadata_schema,
        codegen_schema,
        native_profile};
    identities.codegen = combine(
        "katana-port-codegen-components", codegen_fields);

    const auto runtime_abi =
        std::to_string(
            katana::build_contract::runtime_abi_version);
    const auto block_abi =
        std::to_string(
            katana::build_contract::block_abi_version);
    const auto platform_abi =
        std::to_string(
            katana::build_contract::
                platform_services_abi_version);
    const auto port_contract =
        std::to_string(
            katana::codegen::
                port_project_contract_version);
    const auto native_port_profile_contract =
        std::to_string(
            katana::build_contract::
                native_port_profile_contract_version);
    const auto game_project_contract =
        std::to_string(
            katana::runtime::
                game_project_contract_version);
    const auto game_project_artifact =
        std::to_string(
            katana::runtime::
                game_project_artifact_format_version);
    const std::array<std::string_view, 13u> whole_fields{
        identities.analysis,
        identities.analysis_cache,
        identities.codegen,
        katana::build_contract::ir_component_identity,
        katana::build_contract::
            orchestration_component_identity,
        katana::build_contract::project_version,
        runtime_abi,
        block_abi,
        platform_abi,
        port_contract,
        native_port_profile_contract,
        game_project_contract,
        game_project_artifact};
    identities.whole_export = combine(
        "katana-port-whole-export-components",
        whole_fields);
    return identities;
}

std::string port_export_cache_key(
    const std::string_view source_kind,
    const std::uint32_t source_contract_version,
    const katana::runtime::DiscInstallRecipe& recipe,
    const std::string_view boot_file_name,
    const std::uint32_t entry_address,
    const std::string_view target_name,
    const bool diagnostic_partial,
    const std::string_view console_profile,
    const std::string_view game_project_identity,
    const std::string_view game_entry_handoff_artifact_identity,
    const std::string_view latent_aot_entry_hint_identity,
    const std::string_view analysis_mode_identity,
    const std::string_view implementation_identity,
    const katana::codegen::PartitionOptions& partition_options) {
    std::ostringstream identity;
    const auto append = [&identity](const auto& value) {
        std::ostringstream field;
        field << value;
        append_cache_field(identity, field.str());
    };
    append("katana-port-whole-export");
    append(port_export_cache_version);
    append(source_kind);
    append(source_contract_version);
    append(recipe.version);
    append(port_export_recipe_identity(recipe));
    append(recipe.job_generation);
    append(recipe.content_identity);
    append(recipe.boot_sha256);
    append(boot_file_name);
    append(entry_address);
    append(target_name);
    append(diagnostic_partial);
    append(console_profile);
    append(game_project_identity);
    append(game_entry_handoff_artifact_identity);
    append(latent_aot_entry_hint_identity);
    append(analysis_mode_identity);
    append(partition_options.maximum_functions);
    append(partition_options.maximum_instructions);
    append(implementation_identity);
    append(katana::build_contract::project_version);
    append(katana::build_contract::analyzer_abi_version);
    append(katana::build_contract::runtime_abi_version);
    append(katana::build_contract::block_abi_version);
    append(
        katana::build_contract::
            platform_services_abi_version);
    append(katana::codegen::backend_interface_abi_version);
    append(katana::codegen::port_project_contract_version);
    append(
        katana::build_contract::
            native_port_profile_contract_version);
    append(
        katana::codegen::
            port_partition_emission_schema_version);
    append(
        katana::codegen::
            port_metadata_cache_schema_version);
    append(katana::codegen::codegen_cache_schema_version);
    append(
        katana::codegen::
            native_aot_emission_profile_version);
    append(port_ir_contract_version);
    append(katana::runtime::game_project_contract_version);
    append(
        katana::runtime::
            game_project_artifact_format_version);
    return katana::io::sha256_bytes(identity.str());
}

} // namespace katana::cli
