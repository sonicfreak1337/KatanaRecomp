#pragma once

#include "katana/runtime/game_entry_handoff.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t game_entry_handoff_artifact_format_version = 2u;
inline constexpr std::uint64_t game_entry_handoff_artifact_maximum_size =
    128u * 1024u * 1024u;

enum class GameEntryHandoffArtifactPayloadTargetKind : std::uint8_t {
    MemoryOperation,
    DevicePayload,
};

// A capture payload is bound to a descriptor slot, not to a local path.
// `name` is a stable, title-project-selected label used only inside the
// artifact table; the writer rejects path separators and traversal names.
struct GameEntryHandoffArtifactPayloadTarget {
    GameEntryHandoffArtifactPayloadTargetKind kind =
        GameEntryHandoffArtifactPayloadTargetKind::MemoryOperation;
    std::uint32_t memory_operation_index = 0u;
    GameEntryDeviceKey device;
    std::uint32_t device_field_id = 0u;

    [[nodiscard]] static constexpr GameEntryHandoffArtifactPayloadTarget
    memory_operation(const std::uint32_t index) noexcept {
        GameEntryHandoffArtifactPayloadTarget target;
        target.memory_operation_index = index;
        return target;
    }

    [[nodiscard]] static constexpr GameEntryHandoffArtifactPayloadTarget
    device_payload(const GameEntryDeviceKey key,
                   const std::uint32_t field_id) noexcept {
        GameEntryHandoffArtifactPayloadTarget target;
        target.kind =
            GameEntryHandoffArtifactPayloadTargetKind::DevicePayload;
        target.device = key;
        target.device_field_id = field_id;
        return target;
    }

    [[nodiscard]] bool
    operator==(const GameEntryHandoffArtifactPayloadTarget&) const = default;
};

struct GameEntryHandoffArtifactPayload {
    std::string name;
    GameEntryHandoffArtifactPayloadTarget target;
    std::span<const std::uint8_t> bytes;
};

// Owns the descriptor and the complete validated artifact image. Providers
// therefore never depend on a mutable source file after load() returns.
class GameEntryHandoffArtifact final {
  public:
    GameEntryHandoffArtifact(const GameEntryHandoffArtifact&) = delete;
    GameEntryHandoffArtifact&
    operator=(const GameEntryHandoffArtifact&) = delete;
    GameEntryHandoffArtifact(GameEntryHandoffArtifact&&) = delete;
    GameEntryHandoffArtifact&
    operator=(GameEntryHandoffArtifact&&) = delete;
    ~GameEntryHandoffArtifact() = default;

    [[nodiscard]] static std::shared_ptr<GameEntryHandoffArtifact>
    load(const std::filesystem::path& path);

    // Captures a complete artifact. Private references, payload offsets,
    // slice identities, the artifact identity and descriptor identity are
    // derived by the writer and replace any values in those descriptor slots.
    [[nodiscard]] static std::shared_ptr<GameEntryHandoffArtifact>
    write(const std::filesystem::path& path,
          GameEntryHandoff descriptor,
          std::span<const GameEntryHandoffArtifactPayload> payloads);

    [[nodiscard]] const std::filesystem::path&
    canonical_path() const noexcept;
    [[nodiscard]] const std::string& artifact_identity() const noexcept;
    [[nodiscard]] const GameEntryHandoff& descriptor() const noexcept;
    [[nodiscard]] std::uint64_t file_size() const noexcept;

    // Both callbacks are noexcept and read only this object's owned,
    // prevalidated memory.
    [[nodiscard]] GameEntryHandoffProvider provider() noexcept;

  private:
    struct OwnedSlice {
        std::uint64_t offset = 0u;
        std::uint32_t size = 0u;
        std::string byte_identity;
    };

    GameEntryHandoffArtifact() = default;

    [[nodiscard]] static std::shared_ptr<GameEntryHandoffArtifact>
    load_owned(const std::filesystem::path& path);
    [[nodiscard]] static const GameEntryHandoff*
    describe_callback(void* context,
                      const GameEntryHandoffRequest& request) noexcept;
    [[nodiscard]] static bool
    read_callback(void* context,
                  const GameEntryPrivateSliceReference& reference,
                  std::span<std::uint8_t> destination) noexcept;

    std::filesystem::path canonical_path_;
    std::string artifact_identity_;
    GameEntryHandoff descriptor_;
    std::vector<std::uint8_t> bytes_;
    std::vector<OwnedSlice> slices_;
};

} // namespace katana::runtime
