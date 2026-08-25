#include "port_export_orchestration.hpp"

#include <stdexcept>
#include <string>

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

} // namespace

int main() {
    verified_generation_fields_change_key();
    partial_generation_binding_is_rejected();
    return 0;
}
