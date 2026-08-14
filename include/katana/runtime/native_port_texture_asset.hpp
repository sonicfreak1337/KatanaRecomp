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

class NativePortPlatformServices;
struct NativePortContentFileBinding;

enum class NativePortTextureAssetPixelFormat : std::uint8_t {
    Argb1555 = 0x00u,
    Rgb565 = 0x01u,
    Argb4444 = 0x02u,
};

enum class NativePortTextureAssetDataFormat : std::uint8_t {
    SquareTwiddled = 0x01u,
    SquareTwiddledMipmaps = 0x02u,
    VectorQuantized = 0x03u,
    VectorQuantizedMipmaps = 0x04u,
    Rectangle = 0x09u,
    SmallVectorQuantized = 0x10u,
    SmallVectorQuantizedMipmaps = 0x11u,
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

struct NativePortDecodedTextureMipLevel final {
    NativePortExtent extent;
    std::vector<std::uint8_t> rgba8;
};

struct NativePortDecodedTextureAsset final {
    std::string name;
    std::optional<std::uint32_t> global_index;
    std::uint32_t archive_ordinal = 0u;
    NativePortTextureAssetPixelFormat source_pixel_format =
        NativePortTextureAssetPixelFormat::Rgb565;
    NativePortTextureAssetDataFormat source_data_format =
        NativePortTextureAssetDataFormat::Rectangle;
    NativePortExtent extent;
    // Top level followed by every source-authored lower level. The top level
    // stays in rgba8 for source compatibility; lower_mip_levels is ordered
    // width/2 down to 1x1 and is empty for non-mipmapped source formats.
    std::vector<std::uint8_t> rgba8;
    std::vector<NativePortDecodedTextureMipLevel> lower_mip_levels;
};

// One source-authored texture payload after its containing archive has been
// fully validated. Unlike NativePortDecodedTextureAsset this preserves the
// exact bounded bytes consumed by the source layout. Native SDK adapters use
// it when a later high-level texture upload replaces only part of an existing
// resource; no guest texture-memory or PVR device model is involved.
struct NativePortEncodedTextureAsset final {
    std::string name;
    std::optional<std::uint32_t> global_index;
    std::uint32_t archive_ordinal = 0u;
    NativePortTextureAssetPixelFormat source_pixel_format =
        NativePortTextureAssetPixelFormat::Rgb565;
    NativePortTextureAssetDataFormat source_data_format =
        NativePortTextureAssetDataFormat::Rectangle;
    NativePortExtent extent;
    std::vector<std::uint8_t> encoded;
};

enum class NativePortTextureBackingStoreFailure : std::uint8_t {
    InvalidCapacity,
    InvalidRange,
    ResourceExhausted,
};

class NativePortTextureBackingStoreError final : public std::runtime_error {
  public:
    NativePortTextureBackingStoreError(
        NativePortTextureBackingStoreFailure failure,
        std::uint64_t byte_offset,
        std::string_view operation);

    [[nodiscard]] NativePortTextureBackingStoreFailure failure() const noexcept;
    [[nodiscard]] std::uint64_t byte_offset() const noexcept;

  private:
    NativePortTextureBackingStoreFailure failure_;
    std::uint64_t byte_offset_;
};

enum class NativePortTextureOverlayCoverage : std::uint8_t {
    None,
    Partial,
    Complete,
};

struct NativePortTextureBackingStoreSnapshot final {
    std::uint64_t capacity_bytes = 0u;
    std::uint64_t valid_bytes = 0u;
    std::uint64_t writes = 0u;
};

// Sparse native resource backing for high-level texture upload providers.
// It records only byte ranges explicitly submitted through a native SDK hook.
// Callers supply an identity-bound asset as the destination buffer and overlay
// newer ranges onto it. This preserves partial resource updates without
// interpreting TA packets or exposing a Dreamcast texture-memory device.
class NativePortTextureBackingStore final {
  public:
    explicit NativePortTextureBackingStore(std::size_t capacity_bytes);
    ~NativePortTextureBackingStore();

    NativePortTextureBackingStore(const NativePortTextureBackingStore&) =
        delete;
    NativePortTextureBackingStore& operator=(
        const NativePortTextureBackingStore&) = delete;
    NativePortTextureBackingStore(NativePortTextureBackingStore&&) = delete;
    NativePortTextureBackingStore& operator=(
        NativePortTextureBackingStore&&) = delete;

    void write(std::uint32_t destination,
               std::span<const std::uint8_t> source);
    [[nodiscard]] NativePortTextureOverlayCoverage overlay(
        std::uint32_t source_offset,
        std::span<std::uint8_t> destination) const;
    void clear() noexcept;
    [[nodiscard]] NativePortTextureBackingStoreSnapshot snapshot() const
        noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// The content identity is the verified SHA-256 of the containing content
// object. The source ordinal is always part of the identity because GBIX is
// optional and need not be unique, even inside one content object.
struct NativePortTextureAssetIdentity final {
    std::uint64_t generation = 0u;
    std::optional<std::uint32_t> global_index;
    std::uint32_t archive_ordinal = 0u;
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

// One decoded archive entry after it has been installed in the native GPU
// registry. Containing-content identity plus source ordinal form the stable
// resource identity. The optional GBIX remains descriptive metadata and is
// never assumed unique, even within one archive.
struct NativePortMaterializedTextureAsset final {
    std::string name;
    std::optional<std::uint32_t> global_index;
    std::uint32_t archive_ordinal = 0u;
    std::uint32_t guest_token = 0u;
    NativePortTextureHandle texture;
    NativePortExtent extent;
    std::uint32_t mip_levels = 1u;
};

// Explicit archive lifetime. Title adapters may keep several independently
// verified archives resident and release them at their native SDK unload
// boundary. Entries remain in source order so NJS_TEXLIST-style ordinal
// bindings do not depend on GBIX availability.
struct NativePortMaterializedTextureArchive final {
    std::uint64_t generation = 0u;
    std::array<std::uint8_t, 32u> content_sha256{};
    std::vector<NativePortMaterializedTextureAsset> entries;
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

// Validates the complete archive but copies only the selected entry's exact
// encoded texture payload. This is intentionally separate from materializing
// GPU resources so native SDK adapters can compose partial resource uploads
// before decoding the final texture.
[[nodiscard]] NativePortEncodedTextureAsset
extract_native_port_pvm_texture_archive_entry(
    std::span<const std::uint8_t> source,
    std::uint32_t archive_ordinal,
    const NativePortTextureAssetLimits& limits = {});

[[nodiscard]] NativePortEncodedTextureAsset
extract_native_port_prs_pvm_texture_archive_entry(
    std::span<const std::uint8_t> source,
    std::uint32_t archive_ordinal,
    const NativePortTextureAssetLimits& limits = {});

[[nodiscard]] NativePortEncodedTextureAsset
extract_native_port_prs_pvm_texture_archive_entry(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    std::uint32_t archive_ordinal,
    const NativePortTextureAssetLimits& limits = {});

// Decodes one complete GBIX?/PVRT content object. This is the standalone
// texture counterpart to the PVM archive path and retains the optional GBIX
// identity without exposing a device texture address.
[[nodiscard]] NativePortDecodedTextureAsset decode_native_port_pvr_texture(
    std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits = {});

[[nodiscard]] NativePortDecodedTextureAsset decode_native_port_prs_pvr_texture(
    std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits = {});

// Decodes one exact, headerless texture surface. This is the native boundary
// for SDK-owned embedded assets such as font atlases; title adapters bind the
// source bytes and dimensions by executable/content identity rather than
// reconstructing a guest VRAM upload. The source span must contain exactly
// the canonical encoded payload for the declared layout.
[[nodiscard]] NativePortDecodedTextureAsset
decode_native_port_texture_surface(
    std::span<const std::uint8_t> source,
    NativePortExtent extent,
    NativePortTextureAssetPixelFormat pixel_format,
    NativePortTextureAssetDataFormat data_format,
    const NativePortTextureAssetLimits& limits = {});

// Opens one exact content binding through the native platform boundary,
// decodes a PRS/PVM archive, and acquires every texture as one operation. The
// span overload is for bytes whose identity was already verified by the
// caller; the platform overload performs that verification while opening the
// bound content range.
// Acquisition failure rolls back all completed entries. If graphics-backend
// destruction itself fails during rollback, that cleanup error is reported
// and the caller must invalidate the owning generation before reuse.
[[nodiscard]] NativePortMaterializedTextureArchive
materialize_native_port_prs_pvm_texture_archive(
    std::span<const std::uint8_t> source,
    std::string_view content_byte_identity,
    NativePortTextureRegistry& registry,
    std::uint64_t generation,
    const NativePortTextureAssetLimits& limits = {});

[[nodiscard]] NativePortMaterializedTextureArchive
materialize_native_port_prs_pvm_texture_archive(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    NativePortTextureRegistry& registry,
    std::uint64_t generation,
    const NativePortTextureAssetLimits& limits = {});

// Installs one standalone GBIX?/PVRT object through the same identity,
// generation and rollback contract as an archive. The returned collection
// contains exactly one entry so callers can share release/lifetime handling.
[[nodiscard]] NativePortMaterializedTextureArchive
materialize_native_port_pvr_texture(
    std::span<const std::uint8_t> source,
    std::string_view content_byte_identity,
    NativePortTextureRegistry& registry,
    std::uint64_t generation,
    const NativePortTextureAssetLimits& limits = {});

[[nodiscard]] NativePortMaterializedTextureArchive
materialize_native_port_pvr_texture(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    NativePortTextureRegistry& registry,
    std::uint64_t generation,
    const NativePortTextureAssetLimits& limits = {});

// Releases every reference held by an archive. Successful releases are
// removed even if a later graphics-backend release fails; failed entries stay
// attached to the archive so the caller may retry or invalidate the owning
// generation explicitly.
void release_native_port_texture_archive(
    NativePortTextureRegistry& registry,
    NativePortMaterializedTextureArchive& archive);

} // namespace katana::runtime
