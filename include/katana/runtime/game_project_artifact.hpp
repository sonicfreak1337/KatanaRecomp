#pragma once

#include "katana/runtime/game_project.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t game_project_artifact_format_version = 6u;
inline constexpr std::uint64_t game_project_artifact_maximum_size =
    96u * 1024u * 1024u;

// Owns every string and array referenced by definition(). The artifact is a
// data-only external project descriptor: executable callback pointers and a
// private GameEntryHandoff provider are deliberately attached by the game
// project or CLI instead of being serialized.
class GameProjectArtifact final {
  public:
    GameProjectArtifact(const GameProjectArtifact&) = delete;
    GameProjectArtifact& operator=(const GameProjectArtifact&) = delete;
    GameProjectArtifact(GameProjectArtifact&&) = delete;
    GameProjectArtifact& operator=(GameProjectArtifact&&) = delete;
    ~GameProjectArtifact() = default;

    [[nodiscard]] static std::shared_ptr<GameProjectArtifact>
    load(const std::filesystem::path& path);

    // Fails closed when the definition contains native hooks, function
    // overrides or a GameEntryHandoff. Those contracts contain process-local
    // pointers or separately owned private data and are not portable.
    [[nodiscard]] static std::shared_ptr<GameProjectArtifact>
    write(const std::filesystem::path& path,
          const GameProjectDefinition& definition);

    [[nodiscard]] const std::filesystem::path&
    canonical_path() const noexcept;
    [[nodiscard]] const std::string& artifact_identity() const noexcept;
    [[nodiscard]] const GameProjectDefinition& definition() const noexcept;

  private:
    GameProjectArtifact() = default;

    void rebuild_definition();

    std::filesystem::path canonical_path_;
    std::string artifact_identity_;

    std::uint32_t contract_version_ = game_project_contract_version;
    std::string project_id_;
    std::string project_version_;
    std::string content_identity_;
    std::string boot_file_name_;
    std::string boot_byte_identity_;
    RequiredProductMilestone required_product_milestone_ =
        RequiredProductMilestone::FirstVisibleGameFrame;

    std::vector<std::string> function_symbols_;
    std::vector<std::string> function_image_ids_;
    std::vector<GameProjectFunctionBoundary> function_boundaries_;
    std::vector<std::string> jump_table_image_ids_;
    std::vector<GameProjectJumpTable> jump_tables_;
    std::vector<std::string> callback_table_image_ids_;
    std::vector<GameProjectCallbackTable> callback_tables_;
    std::vector<NativeAotTemplate> runtime_code_templates_;
    std::vector<std::string> symbol_names_;
    std::vector<GameProjectSymbol> symbols_;
    std::vector<std::string> code_identity_values_;
    std::vector<std::string> code_identity_image_ids_;
    std::vector<GameProjectCodeIdentity> code_identities_;
    std::vector<std::string> runtime_image_ids_;
    std::vector<std::string> runtime_image_byte_identities_;
    std::vector<std::vector<std::uint32_t>> runtime_image_entry_offsets_;
    std::vector<GameProjectRuntimeImage> runtime_images_;
    std::optional<DreamcastRuntimeBootConfig> boot_config_;

    GameProjectDefinition definition_;
};

} // namespace katana::runtime
