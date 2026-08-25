#include "port_export_orchestration.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

katana::runtime::DiscInstallRecipe test_recipe() {
    katana::runtime::DiscInstallRecipe recipe;
    recipe.job_generation = std::string(64u, '0');
    recipe.descriptor_sha256 = std::string(64u, '1');
    recipe.content_identity = std::string(64u, '2');
    recipe.boot_sha256 = std::string(64u, '3');
    recipe.tracks.push_back({
        1u,
        0u,
        katana::runtime::GdiTrackType::Data,
        2048u,
        0u,
        1u,
        std::string(64u, '4')});
    return recipe;
}

std::string cache_key(
    const katana::cli::PortExportAnalysisGenerationCacheBinding& binding = {}) {
    return katana::cli::port_export_cache_key(
        "native-disc",
        1u,
        test_recipe(),
        "1ST_READ.BIN",
        0x8c010000u,
        "game",
        false,
        "dreamcast",
        "game-project",
        "handoff",
        "native-port",
        1u,
        "latent-hints",
        "platform-abi",
        "implementation",
        binding);
}

void verified_generation_fields_change_key() {
    const auto unbound = cache_key();
    const auto bound = cache_key({
        "analysis-artifact",
        std::string(64u, 'a'),
        std::string(64u, 'b')});
    const auto other_artifact = cache_key({
        "other-analysis-artifact",
        std::string(64u, 'a'),
        std::string(64u, 'b')});
    const auto other_archive = cache_key({
        "analysis-artifact",
        std::string(64u, 'c'),
        std::string(64u, 'b')});
    const auto other_generation = cache_key({
        "analysis-artifact",
        std::string(64u, 'a'),
        std::string(64u, 'c')});
    if (unbound == bound || bound == other_artifact ||
        bound == other_archive || bound == other_generation)
        throw std::runtime_error(
            "Analyse-Generationsfelder aendern den Cachekey nicht.");
}

void partial_generation_binding_is_rejected() {
    bool rejected = false;
    try {
        static_cast<void>(cache_key({
            "analysis-artifact",
            std::string(64u, 'a'),
            {}}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error(
            "Partielle Analyse-Generationsbindung wurde akzeptiert.");
}

using Adjacency =
    katana::codegen::NativeDiscProgramIndexAdjacency;
using LatentFunction = katana::cli::AgentLatentFunctionAuthority;
using LatentModule = katana::cli::AgentLatentModuleAuthority;

LatentModule latent_module(
    std::string byte_identity,
    std::vector<std::uint32_t> instruction_addresses,
    std::vector<LatentFunction> functions) {
    std::sort(instruction_addresses.begin(), instruction_addresses.end());
    instruction_addresses.erase(
        std::unique(
            instruction_addresses.begin(), instruction_addresses.end()),
        instruction_addresses.end());
    for (auto& function : functions) {
        std::sort(
            function.instruction_addresses.begin(),
            function.instruction_addresses.end());
        std::sort(function.edges.begin(), function.edges.end());
    }
    std::sort(
        functions.begin(), functions.end(),
        [](const auto& left, const auto& right) {
            return left.entry_address < right.entry_address;
        });
    return {
        "module",
        std::move(byte_identity),
        0x1000u,
        0x81000000u,
        {},
        {0u},
        std::move(instruction_addresses),
        std::move(functions)};
}

bool incoming_authority(
    const std::vector<Adjacency>& required,
    const std::vector<Adjacency>& candidate,
    const std::vector<Adjacency>& outgoing,
    const LatentModule& baseline,
    const LatentModule& refreshed,
    const std::vector<Adjacency>& required_outgoing = {}) {
    return katana::cli::agent_program_index_incoming_authority_preserved(
        required,
        candidate,
        required_outgoing.empty() ? outgoing : required_outgoing,
        outgoing,
        {baseline},
        {refreshed});
}

void latent_owner_refinement_is_exact_and_fail_closed() {
    constexpr std::uint32_t owner = 0x81000100u;
    constexpr std::uint32_t refined_owner = 0x81000120u;
    constexpr std::uint32_t sibling_owner = 0x81000130u;
    constexpr std::uint32_t internal_target = 0x81000140u;
    constexpr std::uint32_t external_target = 0x8c010000u;
    const auto baseline = latent_module(
        "sha256:baseline",
        {owner, refined_owner, refined_owner + 2u, sibling_owner,
         internal_target},
        {{owner,
          {owner, refined_owner, refined_owner + 2u, sibling_owner,
           internal_target},
          {{refined_owner, internal_target},
           {refined_owner + 2u, external_target}}}});
    const auto refreshed = latent_module(
        "sha256:baseline",
        {owner, refined_owner, refined_owner + 2u, sibling_owner,
         internal_target},
        {{owner, {owner}, {}},
         {refined_owner,
          {refined_owner, refined_owner + 2u, internal_target},
          {{refined_owner, internal_target},
           {refined_owner + 2u, external_target}}},
         {sibling_owner, {sibling_owner}}});

    if (!incoming_authority(
            {{internal_target, {owner}}},
            {{internal_target, {refined_owner}}},
            {},
            baseline,
            refreshed))
        throw std::runtime_error(
            "Exakte Latent-Owner-Verfeinerung wurde verworfen.");
    if (!incoming_authority(
            {{external_target, {owner}}},
            {{external_target, {refined_owner}}},
            {{refined_owner, {external_target}}},
            baseline,
            refreshed))
        throw std::runtime_error(
            "Externe Kante der Latent-Owner-Verfeinerung ging verloren.");

    auto foreign = refreshed;
    foreign.byte_identity = "sha256:foreign";
    if (incoming_authority(
            {{internal_target, {owner}}},
            {{internal_target, {refined_owner}}},
            {},
            baseline,
            foreign))
        throw std::runtime_error(
            "Fremde Modulidentitaet wurde als Owner-Verfeinerung akzeptiert.");
    if (incoming_authority(
            {{internal_target, {owner}}},
            {{internal_target, {0x81000180u}}},
            {},
            baseline,
            refreshed))
        throw std::runtime_error(
            "Owner ausserhalb des alten Funktionskoerpers wurde akzeptiert.");
    if (incoming_authority(
            {{internal_target, {owner}}},
            {},
            {},
            baseline,
            refreshed))
        throw std::runtime_error(
            "Fehlende Zielkante wurde als Verfeinerung akzeptiert.");
    auto ambiguous_refreshed = refreshed;
    ambiguous_refreshed.functions[2].instruction_addresses.push_back(
        refined_owner);
    std::sort(
        ambiguous_refreshed.functions[2].instruction_addresses.begin(),
        ambiguous_refreshed.functions[2].instruction_addresses.end());
    ambiguous_refreshed.functions[2].edges = {
        {refined_owner, internal_target}};
    if (incoming_authority(
            {{internal_target, {owner}}},
            {{internal_target, {refined_owner, sibling_owner}}},
            {},
            baseline,
            ambiguous_refreshed))
        throw std::runtime_error(
            "Mehrdeutige Latent-Owner-Verfeinerung wurde akzeptiert.");
    if (incoming_authority(
            {{external_target, {owner}}},
            {{external_target, {refined_owner}}},
            {},
            baseline,
            refreshed))
        throw std::runtime_error(
            "Externe Kante ohne gefilterte Outgoing-Evidenz wurde akzeptiert.");
    auto moved_site = refreshed;
    moved_site.functions[1].edges = {
        {refined_owner + 4u, internal_target},
        {refined_owner + 2u, external_target}};
    if (incoming_authority(
            {{internal_target, {owner}}},
            {{internal_target, {refined_owner}}},
            {},
            baseline,
            moved_site))
        throw std::runtime_error(
            "Verlorene kantenlokale Site-Evidenz wurde akzeptiert.");

    auto bound_baseline = baseline;
    bound_baseline.source_bindings.push_back({
        "disc-extent",
        katana::codegen::LatentAotSourceTransform::Identity,
        "sha256:source",
        0x2000u,
        0x1000u});
    if (incoming_authority(
            {{internal_target, {owner}}},
            {{internal_target, {refined_owner}}},
            {},
            bound_baseline,
            refreshed))
        throw std::runtime_error(
            "Verlorene Latent-Source-Bindung wurde akzeptiert.");

    auto duplicate_function = refreshed;
    duplicate_function.functions.push_back(
        duplicate_function.functions[1]);
    std::sort(
        duplicate_function.functions.begin(),
        duplicate_function.functions.end(),
        [](const auto& left, const auto& right) {
            return left.entry_address < right.entry_address;
        });
    if (incoming_authority(
            {{internal_target, {owner}}},
            {{internal_target, {refined_owner}}},
            {},
            baseline,
            duplicate_function))
        throw std::runtime_error(
            "Mehrdeutige Latent-Funktionsidentitaet wurde akzeptiert.");

    if (katana::cli::agent_program_index_incoming_authority_preserved(
            {{internal_target, {owner}}},
            {{internal_target, {refined_owner}}},
            {},
            {},
            {baseline},
            {refreshed, refreshed}))
        throw std::runtime_error(
            "Mehrdeutige exakte Modulidentitaet wurde akzeptiert.");
}

void latent_owner_refinement_preserves_matching_outgoing_edge_only() {
    constexpr std::uint32_t owner = 0x81000100u;
    constexpr std::uint32_t refined_owner = 0x81000120u;
    constexpr std::uint32_t target = 0x8c010000u;
    constexpr std::uint32_t reachable_target = 0x8c010100u;
    const auto baseline = latent_module(
        "sha256:outgoing",
        {owner, refined_owner},
        {{owner,
          {owner, refined_owner},
          {{refined_owner, target}}}});
    const auto refreshed = latent_module(
        "sha256:outgoing",
        {owner, refined_owner},
        {{owner, {owner}, {}},
         {refined_owner,
          {refined_owner},
          {{refined_owner, target}}}});
    const std::vector<Adjacency> outgoing = {
        {refined_owner, {target}}};
    if (!incoming_authority(
            {{target, {owner}}},
            {{target, {owner}}},
            outgoing,
            baseline,
            refreshed))
        throw std::runtime_error(
            "Zielgebundener Latent-Owner-Beleg wurde nicht erzeugt.");
    if (!katana::cli::agent_latent_outgoing_owner_refinement_preserved(
            outgoing, owner, target, {baseline}, {refreshed}))
        throw std::runtime_error(
            "Passende verfeinerte Outgoing-Kante wurde verworfen.");
    const std::vector<Adjacency> split_outgoing = {
        {owner, {refined_owner}},
        {refined_owner, {target, reachable_target}}};
    if (katana::cli::agent_latent_outgoing_owner_refinement_preserved(
            split_outgoing,
            owner,
            reachable_target,
            {baseline},
            {refreshed}))
        throw std::runtime_error(
            "Function-Split ohne zielgebundene Site-Evidenz wurde akzeptiert.");
    if (katana::cli::agent_latent_outgoing_owner_refinement_preserved(
            {}, owner, target, {baseline}, {refreshed}) ||
        katana::cli::agent_latent_outgoing_owner_refinement_preserved(
            outgoing,
            owner,
            target + 2u,
            {baseline},
            {refreshed}))
        throw std::runtime_error(
            "Fehlende oder zielverschobene Outgoing-Kante wurde akzeptiert.");
    auto ambiguous = refreshed;
    ambiguous.functions.push_back(refreshed.functions.back());
    if (katana::cli::agent_latent_outgoing_owner_refinement_preserved(
            outgoing, owner, target, {baseline}, {ambiguous}))
        throw std::runtime_error(
            "Mehrdeutiger Outgoing-Owner-Beleg wurde akzeptiert.");

    auto unproven = refreshed;
    unproven.functions.back().edges.clear();
    if (!incoming_authority(
            {{target, {owner}}},
            {{target, {owner}}},
            {},
            baseline,
            unproven))
        throw std::runtime_error(
            "Unveraenderte reine Incoming-Evidenz wurde verworfen.");
    if (incoming_authority(
            {{target, {owner}}},
            {{target, {owner}}},
            {},
            baseline,
            unproven,
            {{owner, {target}}}))
        throw std::runtime_error(
            "Retained Incoming ohne exakten Outgoing-Witness wurde akzeptiert.");
}

void retained_filtered_edge_survives_coarse_owner_reassignment() {
    constexpr std::uint32_t owner = 0x81000100u;
    constexpr std::uint32_t replacement = 0x81000200u;
    constexpr std::uint32_t target = 0x81000140u;
    const auto baseline = latent_module(
        "sha256:stable",
        {owner, target},
        {{owner, {owner, target}, {{owner, target}}}});
    const auto refreshed = latent_module(
        "sha256:stable",
        {owner, replacement, target},
        {{owner, {owner, target}, {{owner, target}}},
         {replacement, {replacement}}});
    if (!incoming_authority(
            {{target, {owner}}},
            {{target, {replacement}}},
            {{owner, {target}}},
            baseline,
            refreshed))
        throw std::runtime_error(
            "Erhaltene gefilterte Owner-Kante wurde vom groben Incoming-Index verworfen.");
    auto changed_site = refreshed;
    changed_site.functions.front().edges = {{owner + 2u, target}};
    if (incoming_authority(
            {{target, {owner}}},
            {{target, {replacement}}},
            {{owner, {target}}},
            baseline,
            changed_site))
        throw std::runtime_error(
            "Gefilterte Kante ohne erhaltene Site-Evidenz wurde akzeptiert.");
}

void exact_primary_entry_remap_is_preserved() {
    constexpr std::uint32_t target = 0x8c010200u;
    constexpr std::uint32_t old_entry = 0x8c010100u;
    constexpr std::uint32_t owner = 0x8c010000u;
    if (!katana::cli::agent_program_index_incoming_authority_preserved(
            {{target, {old_entry}}},
            {{target, {owner}}},
            {},
            {},
            {},
            {},
            {{old_entry, owner}}))
        throw std::runtime_error(
            "Exakter Primary-Entry-Remap ging verloren.");
    if (katana::cli::agent_program_index_incoming_authority_preserved(
            {{target, {old_entry}}},
            {{target, {owner + 2u}}},
            {},
            {},
            {},
            {},
            {{old_entry, owner}}))
        throw std::runtime_error(
            "Falscher Primary-Entry-Remap wurde akzeptiert.");
}

} // namespace

int main() {
    try {
        verified_generation_fields_change_key();
        partial_generation_binding_is_rejected();
        latent_owner_refinement_is_exact_and_fail_closed();
        latent_owner_refinement_preserves_matching_outgoing_edge_only();
        retained_filtered_edge_survives_coarse_owner_reassignment();
        exact_primary_entry_remap_is_preserved();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
