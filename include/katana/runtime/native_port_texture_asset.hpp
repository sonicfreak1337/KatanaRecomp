#pragma once

#include "katana/runtime/native_port_graphics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

enum class NativePortTextureAssetPixelFormat : std::uint8_t {
    Argb1555 = 0x00u,
    Rgb565 = 0x01u,
    Argb4444 = 0x02u,
};

enum class NativePortTextureAssetDataFormat : std::uint8_t {
    SquareTwiddled = 0x01u,
    Rectangle = 0x09u,
};

enum class NativePortTextureAssetFailure : std::uint8_t {
    InvalidLimits,
    CompressedInputLimit,
    InvalidPrs,
    DecompressedOutputLimit,
    InvalidPvm,
    UnsupportedPvmFeature,
    ArchiveEntryLimit,
    InvalidPvrt,
    UnsupportedPixelFormat,
    UnsupportedDataFormat,
    InvalidDimensions,
    RgbaOutputLimit,
    ResourceExhausted,
};

class NativePortTextureAssetError final : public std::runtime_error {
  public:
    NativePortTextureAssetError(NativePortTextureAssetFailure failure,
                                std::uint64_t byte_offset,
                                std::string_view operation);

    [[nodiscard]] NativePortTextureAssetFailure failure() const noexcept;
    [[nodiscard]] std::uint64_t byte_offset() const noexcept;

  private:
    NativePortTextureAssetFailure failure_;
    std::uint64_t byte_offset_;
};

// All limits are aggregate limits for one operation. PVR texture dimensions
// are additionally constrained to the hardware format's 1024-pixel maximum.
struct NativePortTextureAssetLimits final {
    std::size_t maximum_compressed_bytes = 64u * 1024u * 1024u;
    std::size_t maximum_decompressed_bytes = 512u * 1024u * 1024u;
    std::size_t maximum_archive_entries = 4'096u;
    std::uint32_t maximum_dimension = 1'024u;
    std::size_t maximum_rgba_bytes = 512u * 1024u * 1024u;
};

struct NativePortDecodedTextureAsset final {
    std::string name;
    std::optional<std::uint32_t> global_index;
    NativePortTextureAssetPixelFormat source_pixel_format =
        NativePortTextureAssetPixelFormat::Rgb565;
    NativePortTextureAssetDataFormat source_data_format =
        NativePortTextureAssetDataFormat::Rectangle;
    NativePortExtent extent;
    std::vector<std::uint8_t> rgba8;
};

// The content identity is the verified SHA-256 of the containing content
// object. global_index selects an entry within a multi-texture object; callers
// without a global index must instead provide a per-texture content identity.
struct NativePortTextureAssetIdentity final {
    std::uint64_t generation = 0u;
    std::optional<std::uint32_t> global_index;
    std::array<std::uint8_t, 32u> content_sha256{};

    friend constexpr bool operator==(const NativePortTextureAssetIdentity&,
                                     const NativePortTextureAssetIdentity&) =
        default;
};

enum class NativePortTextureRegistryFailure : std::uint8_t {
    InvalidConfig,
    InvalidIdentity,
    InvalidTexture,
    IdentityCollision,
    TokenExhausted,
    UnknownToken,
    GenerationMismatch,
    ReferenceCountLimit,
    EntryLimit,
    ByteLimit,
    ResourceExhausted,
};

class NativePortTextureRegistryError final : public std::runtime_error {
  public:
    NativePortTextureRegistryError(NativePortTextureRegistryFailure failure,
                                   std::uint32_t guest_token,
                                   std::string_view operation);

    [[nodiscard]] NativePortTextureRegistryFailure failure() const noexcept;
    [[nodiscard]] std::uint32_t guest_token() const noexcept;

  private:
    NativePortTextureRegistryFailure failure_;
    std::uint32_t guest_token_;
};

struct NativePortTextureRegistryLimits final {
    std::uint32_t maximum_entries = 4'096u;
    std::uint64_t maximum_texture_bytes = 1ull << 30u;
};

struct NativePortTextureRegistryBinding final {
    std::uint32_t guest_token = 0u;
    NativePortTextureHandle texture;
};

struct NativePortTextureRegistrySnapshot final {
    std::uint32_t entries = 0u;
    std::uint64_t texture_bytes = 0u;
    std::uint64_t acquired_references = 0u;
};

// Owns the GPU handles registered through it. Tokens are opaque, non-zero and
// never reused during a registry lifetime. All methods, including destruction,
// must run on the NativePortGraphicsDevice construction thread. Explicit
// invalidation reports graphics destruction failures; the destructor performs
// only best-effort cleanup before the graphics device releases its own slots.
class NativePortTextureRegistry final {
  public:
    explicit NativePortTextureRegistry(
        NativePortGraphicsDevice& graphics,
        const NativePortTextureRegistryLimits& limits = {});
    ~NativePortTextureRegistry();

    NativePortTextureRegistry(const NativePortTextureRegistry&) = delete;
    NativePortTextureRegistry& operator=(const NativePortTextureRegistry&) =
        delete;
    NativePortTextureRegistry(NativePortTextureRegistry&&) = delete;
    NativePortTextureRegistry& operator=(NativePortTextureRegistry&&) = delete;

    [[nodiscard]] NativePortTextureRegistryBinding acquire(
        const NativePortTextureAssetIdentity& identity,
        const NativePortDecodedTextureAsset& texture);
    [[nodiscard]] NativePortTextureHandle resolve(
        std::uint32_t guest_token,
        std::uint64_t expected_generation) const;
    void release(std::uint32_t guest_token,
                 std::uint64_t expected_generation);
    void invalidate_generation(std::uint64_t generation);
    void invalidate_all();

    [[nodiscard]] NativePortTextureRegistrySnapshot snapshot() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// The stream must end with its defined zero-offset token exactly at the input
// boundary. Input and output remain bounded by the supplied span and limits.
[[nodiscard]] std::vector<std::uint8_t> decompress_native_port_prs(
    std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits = {});

[[nodiscard]] std::vector<NativePortDecodedTextureAsset>
decode_native_port_pvm_texture_archive(
    std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits = {});

[[nodiscard]] std::vector<NativePortDecodedTextureAsset>
decode_native_port_prs_pvm_texture_archive(
    std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits = {});

} // namespace katana::runtime
