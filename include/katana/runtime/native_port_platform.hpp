#pragma once

#include "katana/runtime/native_port.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_platform_contract_version = 4u;
inline constexpr std::size_t native_port_gamepad_count = 4u;

struct NativePortPlatformConfig final {
    std::uint32_t contract_version = native_port_platform_contract_version;
    // Read-only original/title content and writable product state are
    // deliberately separate roots. Both are validated component-by-component
    // and may not overlap.
    std::filesystem::path content_root;
    std::filesystem::path user_data_root;
    std::string_view project_id;
    std::uint64_t maximum_content_file_bytes = 16ull * 1024u * 1024u * 1024u;
    std::uint32_t maximum_save_payload_bytes = 16u * 1024u * 1024u;
    bool require_gamepad_backend = true;
};

enum class NativePortPlatformFailure : std::uint8_t {
    InvalidConfig,
    UnsupportedHost,
    ThreadViolation,
    ContentBoundary,
    ContentIdentity,
    ContentRead,
    InputBackend,
    InvalidController,
    InvalidVibration,
    SaveBoundary,
    SaveCorrupt,
    SaveIncompatible,
    SaveConflict,
    SaveRead,
    SaveWrite,
    ResourceLimit,
};

class NativePortPlatformError final : public std::runtime_error {
  public:
    NativePortPlatformError(NativePortPlatformFailure failure,
                            std::uint32_t platform_error_code,
                            std::string_view operation);

    [[nodiscard]] NativePortPlatformFailure failure() const noexcept;
    [[nodiscard]] std::uint32_t platform_error_code() const noexcept;

  private:
    NativePortPlatformFailure failure_;
    std::uint32_t platform_error_code_;
};

// A title adapter binds a logical native file to one exact byte range from
// the user's local original-content tree. It may describe a pre-extracted
// file or a verified contiguous range in another local source; no retail
// bytes enter the generated port package.
struct NativePortContentFileBinding final {
    std::string_view logical_id;
    std::filesystem::path content_relative_path;
    std::string_view byte_identity;
    std::uint64_t source_offset = 0u;
    std::uint64_t byte_size = 0u;
};

struct NativePortContentFileSnapshot final {
    std::uint64_t byte_size = 0u;
    std::uint64_t read_operations = 0u;
    std::uint64_t bytes_read = 0u;
};

// Locked, identity-verified random access to one bound content range. The
// source handle denies write/delete sharing for its complete lifetime.
class NativePortReadOnlyFile final {
  public:
    ~NativePortReadOnlyFile();

    NativePortReadOnlyFile(const NativePortReadOnlyFile&) = delete;
    NativePortReadOnlyFile& operator=(const NativePortReadOnlyFile&) = delete;
    NativePortReadOnlyFile(NativePortReadOnlyFile&&) = delete;
    NativePortReadOnlyFile& operator=(NativePortReadOnlyFile&&) = delete;

    [[nodiscard]] std::string_view logical_id() const noexcept;
    [[nodiscard]] std::uint64_t byte_size() const noexcept;
    void read_at(std::uint64_t offset, std::span<std::byte> destination);
    [[nodiscard]] NativePortContentFileSnapshot snapshot() const;

  private:
    class Impl;
    explicit NativePortReadOnlyFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class NativePortPlatformServices;
};

enum class NativePortGamepadButton : std::uint32_t {
    DpadUp = 1u << 0u,
    DpadDown = 1u << 1u,
    DpadLeft = 1u << 2u,
    DpadRight = 1u << 3u,
    Menu = 1u << 4u,
    View = 1u << 5u,
    LeftStick = 1u << 6u,
    RightStick = 1u << 7u,
    LeftShoulder = 1u << 8u,
    RightShoulder = 1u << 9u,
    A = 1u << 10u,
    B = 1u << 11u,
    X = 1u << 12u,
    Y = 1u << 13u,
};

[[nodiscard]] constexpr std::uint32_t native_port_gamepad_button_mask(
    const NativePortGamepadButton button) noexcept {
    return static_cast<std::uint32_t>(button);
}

// Stick axes use a backend-independent coordinate system: left/down is -1,
// right/up is +1. Face buttons describe their physical layout (A=south,
// B=east, X=west, Y=north), so XInput, DualSense/DualShock and future Linux
// backends feed the same title-facing contract.
struct NativePortGamepadState final {
    bool connected = false;
    std::uint32_t packet_number = 0u;
    std::uint32_t buttons = 0u;
    std::int16_t left_stick_x_raw = 0;
    std::int16_t left_stick_y_raw = 0;
    std::int16_t right_stick_x_raw = 0;
    std::int16_t right_stick_y_raw = 0;
    std::uint8_t left_trigger_raw = 0u;
    std::uint8_t right_trigger_raw = 0u;
    float left_stick_x = 0.0f;
    float left_stick_y = 0.0f;
    float right_stick_x = 0.0f;
    float right_stick_y = 0.0f;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
};

struct NativePortInputSnapshot final {
    std::uint64_t poll_sequence = 0u;
    std::uint64_t connection_generation = 0u;
    std::array<NativePortGamepadState, native_port_gamepad_count> gamepads{};
};

struct NativePortGamepadVibration final {
    float low_frequency = 0.0f;
    float high_frequency = 0.0f;
};

struct NativePortSaveKey final {
    std::string_view slot_id;
    std::uint32_t schema_version = 0u;
};

enum class NativePortSaveLoadStatus : std::uint8_t {
    Missing,
    Loaded,
    RecoveredFromBackup,
    IncompatibleSchema,
    Corrupt,
};

struct NativePortSaveLoadResult final {
    NativePortSaveLoadStatus status = NativePortSaveLoadStatus::Missing;
    std::uint32_t stored_schema_version = 0u;
    std::uint64_t generation = 0u;
    std::vector<std::byte> payload;
};

struct NativePortPlatformSnapshot final {
    std::uint64_t content_open_operations = 0u;
    std::uint64_t content_bytes_verified = 0u;
    std::uint64_t content_read_operations = 0u;
    std::uint64_t content_bytes_read = 0u;
    std::uint64_t input_polls = 0u;
    std::uint64_t input_connection_generation = 0u;
    std::uint64_t save_load_operations = 0u;
    std::uint64_t save_store_operations = 0u;
    std::uint64_t save_bytes_read = 0u;
    std::uint64_t save_bytes_written = 0u;
    std::uint32_t last_platform_error_code = 0u;
    bool native_file_backend = false;
    bool native_gamepad_backend = false;
    bool native_save_backend = false;
};

// Native title/platform boundary. It exposes ordinary host files, gamepads and
// atomic save records; it never exposes GD-ROM, Maple, VMU, flash-device or
// guest-DMA protocols. All calls and destruction are owner-thread confined.
class NativePortPlatformServices final {
  public:
    explicit NativePortPlatformServices(const NativePortPlatformConfig& config);
    ~NativePortPlatformServices();

    NativePortPlatformServices(const NativePortPlatformServices&) = delete;
    NativePortPlatformServices& operator=(const NativePortPlatformServices&) = delete;
    NativePortPlatformServices(NativePortPlatformServices&&) = delete;
    NativePortPlatformServices& operator=(NativePortPlatformServices&&) = delete;

    [[nodiscard]] const std::filesystem::path& content_root() const;
    [[nodiscard]] const std::filesystem::path& user_data_root() const;

    [[nodiscard]] std::unique_ptr<NativePortReadOnlyFile> open_content_file(
        const NativePortContentFileBinding& binding);

    [[nodiscard]] NativePortInputSnapshot poll_gamepads();
    [[nodiscard]] bool set_gamepad_vibration(
        std::uint32_t controller_index,
        const NativePortGamepadVibration& vibration);

    [[nodiscard]] NativePortSaveLoadResult load_save(
        const NativePortSaveKey& key);
    [[nodiscard]] std::uint64_t store_save(
        const NativePortSaveKey& key,
        std::span<const std::byte> payload);

    [[nodiscard]] NativePortPlatformSnapshot snapshot() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime
