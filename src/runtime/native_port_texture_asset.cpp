#include "katana/runtime/native_port_texture_asset.hpp"

#include "katana/runtime/native_port.hpp"
#include "katana/runtime/native_port_platform.hpp"

#include "prs_decode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint32_t pvm_magic = 0x484D5650u;
constexpr std::uint32_t pvrt_magic = 0x54525650u;
constexpr std::uint32_t gbix_magic = 0x58494247u;
constexpr std::uint16_t pvm_filename_flag = 0x0001u;
constexpr std::uint16_t pvm_pixel_data_format_flag = 0x0002u;
constexpr std::uint16_t pvm_texture_dimensions_flag = 0x0004u;
constexpr std::uint16_t pvm_global_index_flag = 0x0008u;
constexpr std::uint16_t pvm_pvrt_flag = 0x0100u;
constexpr std::uint16_t supported_pvm_flags =
    pvm_filename_flag | pvm_pixel_data_format_flag |
    pvm_texture_dimensions_flag | pvm_global_index_flag | pvm_pvrt_flag;
constexpr std::size_t pvm_name_bytes = 28u;
constexpr std::uint32_t maximum_pvr_dimension = 1'024u;
constexpr std::uint32_t minimum_pvr_dimension = 8u;

[[noreturn]] void fail(const NativePortTextureAssetFailure failure,
                       const std::size_t offset,
                       const std::string_view operation) {
    throw NativePortTextureAssetError(
        failure, static_cast<std::uint64_t>(offset), operation);
}

[[nodiscard]] std::size_t checked_add(
    const std::size_t left,
    const std::size_t right,
    const NativePortTextureAssetFailure failure,
    const std::size_t offset,
    const std::string_view operation) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        fail(failure, offset, operation);
    return left + right;
}

[[nodiscard]] std::size_t checked_multiply(
    const std::size_t left,
    const std::size_t right,
    const NativePortTextureAssetFailure failure,
    const std::size_t offset,
    const std::string_view operation) {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
        fail(failure, offset, operation);
    return left * right;
}

void validate_limits(const NativePortTextureAssetLimits& limits) {
    if (limits.maximum_compressed_bytes == 0u ||
        limits.maximum_decompressed_bytes == 0u ||
        limits.maximum_archive_entries == 0u ||
        limits.maximum_archive_entries >
            std::numeric_limits<std::uint16_t>::max() ||
        limits.maximum_dimension < minimum_pvr_dimension ||
        limits.maximum_dimension > maximum_pvr_dimension ||
        limits.maximum_rgba_bytes == 0u)
        fail(NativePortTextureAssetFailure::InvalidLimits, 0u, "limits");
}

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::uint8_t> source,
    const std::size_t offset,
    const NativePortTextureAssetFailure failure,
    const std::string_view operation) {
    if (offset > source.size() || source.size() - offset < 2u)
        fail(failure, offset, operation);
    return static_cast<std::uint16_t>(source[offset]) |
           static_cast<std::uint16_t>(source[offset + 1u] << 8u);
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::uint8_t> source,
    const std::size_t offset,
    const NativePortTextureAssetFailure failure,
    const std::string_view operation) {
    if (offset > source.size() || source.size() - offset < 4u)
        fail(failure, offset, operation);
    return static_cast<std::uint32_t>(source[offset]) |
           (static_cast<std::uint32_t>(source[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(source[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(source[offset + 3u]) << 24u);
}

[[nodiscard]] bool all_zero(const std::span<const std::uint8_t> source) {
    return std::ranges::all_of(source,
                               [](const std::uint8_t value) {
                                   return value == 0u;
                               });
}

[[nodiscard]] std::vector<std::uint8_t> decompress_prs_impl(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
    validate_limits(limits);
    try {
        return katana::detail::decompress_sega_prs(
            source, limits.maximum_compressed_bytes,
            limits.maximum_decompressed_bytes);
    } catch (const katana::detail::PrsDecodeError& error) {
        auto failure = NativePortTextureAssetFailure::InvalidPrs;
        switch (error.failure()) {
        case katana::detail::PrsDecodeFailure::InvalidLimits:
            failure = NativePortTextureAssetFailure::InvalidLimits;
            break;
        case katana::detail::PrsDecodeFailure::InvalidInput:
            failure = NativePortTextureAssetFailure::InvalidPrs;
            break;
        case katana::detail::PrsDecodeFailure::CompressedInputLimit:
            failure = NativePortTextureAssetFailure::CompressedInputLimit;
            break;
        case katana::detail::PrsDecodeFailure::DecompressedOutputLimit:
            failure = NativePortTextureAssetFailure::DecompressedOutputLimit;
            break;
        }
        fail(failure, error.source_offset(), error.operation());
    }
}

struct PvmEntryMetadata final {
    std::string name;
    std::optional<std::uint32_t> global_index;
    std::optional<std::uint8_t> pixel_format;
    std::optional<std::uint8_t> data_format;
    std::optional<std::uint16_t> dimensions;
};

[[nodiscard]] NativePortTextureAssetPixelFormat parse_pixel_format(
    const std::uint8_t value,
    const std::size_t offset) {
    switch (value) {
    case 0x00u:
        return NativePortTextureAssetPixelFormat::Argb1555;
    case 0x01u:
        return NativePortTextureAssetPixelFormat::Rgb565;
    case 0x02u:
        return NativePortTextureAssetPixelFormat::Argb4444;
    default:
        fail(NativePortTextureAssetFailure::UnsupportedPixelFormat, offset,
             "pvrt-pixel-format");
    }
}

[[nodiscard]] NativePortTextureAssetDataFormat parse_data_format(
    const std::uint8_t value,
    const std::size_t offset) {
    switch (value) {
    case 0x01u:
        return NativePortTextureAssetDataFormat::SquareTwiddled;
    case 0x02u:
        return NativePortTextureAssetDataFormat::SquareTwiddledMipmaps;
    case 0x03u:
        return NativePortTextureAssetDataFormat::VectorQuantized;
    case 0x04u:
        return NativePortTextureAssetDataFormat::VectorQuantizedMipmaps;
    case 0x09u:
        return NativePortTextureAssetDataFormat::Rectangle;
    case 0x10u:
        return NativePortTextureAssetDataFormat::SmallVectorQuantized;
    case 0x11u:
        return NativePortTextureAssetDataFormat::SmallVectorQuantizedMipmaps;
    default:
        fail(NativePortTextureAssetFailure::UnsupportedDataFormat, offset,
             "pvrt-data-format");
    }
}

void validate_dimensions(const NativePortExtent extent,
                         const NativePortTextureAssetDataFormat data_format,
                         const NativePortTextureAssetLimits& limits,
                         const std::size_t offset) {
    const bool basic_valid =
        extent.width >= minimum_pvr_dimension &&
        extent.height >= minimum_pvr_dimension &&
        extent.width <= limits.maximum_dimension &&
        extent.height <= limits.maximum_dimension &&
        std::has_single_bit(extent.width) && std::has_single_bit(extent.height);
    const auto requires_square =
        data_format != NativePortTextureAssetDataFormat::Rectangle;
    if (!basic_valid || (requires_square && extent.width != extent.height))
        fail(NativePortTextureAssetFailure::InvalidDimensions, offset,
             "pvrt-dimensions");
}

[[nodiscard]] bool is_twiddled(
    const NativePortTextureAssetDataFormat format) noexcept {
    return format == NativePortTextureAssetDataFormat::SquareTwiddled ||
           format ==
               NativePortTextureAssetDataFormat::SquareTwiddledMipmaps;
}

[[nodiscard]] bool is_vector_quantized(
    const NativePortTextureAssetDataFormat format) noexcept {
    return format == NativePortTextureAssetDataFormat::VectorQuantized ||
           format ==
               NativePortTextureAssetDataFormat::VectorQuantizedMipmaps ||
           format ==
               NativePortTextureAssetDataFormat::SmallVectorQuantized ||
           format == NativePortTextureAssetDataFormat::
                         SmallVectorQuantizedMipmaps;
}

[[nodiscard]] bool has_mipmaps(
    const NativePortTextureAssetDataFormat format) noexcept {
    return format ==
               NativePortTextureAssetDataFormat::SquareTwiddledMipmaps ||
           format ==
               NativePortTextureAssetDataFormat::VectorQuantizedMipmaps ||
           format == NativePortTextureAssetDataFormat::
                         SmallVectorQuantizedMipmaps;
}

// The native-port runtime is intentionally linkable without the historical
// diagnostic/core runtime.  Keep this fixed-size SHA-256 accumulator local to
// the texture asset TU instead of introducing a cross-runtime dependency just
// for the one-time materialization identity.
class TexturePayloadSha256Accumulator final {
  public:
    void update(const std::string_view bytes) noexcept {
        const auto* cursor = reinterpret_cast<const std::uint8_t*>(
            bytes.data());
        auto remaining = bytes.size();
        if (buffer_size_ != 0u) {
            const auto copied =
                std::min(buffer_.size() - buffer_size_, remaining);
            std::memcpy(buffer_.data() + buffer_size_, cursor, copied);
            buffer_size_ += copied;
            cursor += copied;
            remaining -= copied;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_.data());
                buffer_size_ = 0u;
            }
        }
        while (remaining >= buffer_.size()) {
            transform(cursor);
            cursor += buffer_.size();
            remaining -= buffer_.size();
        }
        if (remaining != 0u) {
            std::memcpy(buffer_.data(), cursor, remaining);
            buffer_size_ = remaining;
        }
        total_bytes_ += static_cast<std::uint64_t>(bytes.size());
    }

    [[nodiscard]] NativePortTexturePayloadSha256 finish() noexcept {
        buffer_[buffer_size_++] = 0x80u;
        if (buffer_size_ > 56u) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(
                          buffer_size_),
                      buffer_.end(), std::uint8_t{0u});
            transform(buffer_.data());
            buffer_size_ = 0u;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                  buffer_.begin() + 56, std::uint8_t{0u});
        const auto bit_count = total_bytes_ * 8u;
        for (std::size_t index = 0u; index < sizeof(bit_count); ++index)
            buffer_[63u - index] = static_cast<std::uint8_t>(
                bit_count >> (index * 8u));
        transform(buffer_.data());

        NativePortTexturePayloadSha256 digest{};
        for (std::size_t word = 0u; word < state_.size(); ++word) {
            for (std::size_t byte = 0u; byte < sizeof(std::uint32_t);
                 ++byte)
                digest[word * sizeof(std::uint32_t) + byte] =
                    static_cast<std::uint8_t>(
                        state_[word] >> (24u - byte * 8u));
        }
        return digest;
    }

  private:
    void transform(const std::uint8_t* const block) noexcept {
        std::array<std::uint32_t, 64u> words{};
        for (std::size_t index = 0u; index < 16u; ++index) {
            const auto offset = index * 4u;
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24u) |
                (static_cast<std::uint32_t>(block[offset + 1u]) << 16u) |
                (static_cast<std::uint32_t>(block[offset + 2u]) << 8u) |
                static_cast<std::uint32_t>(block[offset + 3u]);
        }
        for (std::size_t index = 16u; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15u], 7) ^
                            std::rotr(words[index - 15u], 18) ^
                            (words[index - 15u] >> 3u);
            const auto s1 = std::rotr(words[index - 2u], 17) ^
                            std::rotr(words[index - 2u], 19) ^
                            (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0u; index < words.size(); ++index) {
            const auto sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                                std::rotr(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto first = h + sigma1 + choice + round_constants[index] +
                               words[index];
            const auto sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                                std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto second = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    static constexpr std::array<std::uint32_t, 64u> round_constants{
        0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
        0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
        0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
        0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
        0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
        0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
        0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
        0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
        0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
        0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
        0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
        0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
        0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
        0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
        0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
        0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u};

    std::array<std::uint32_t, 8u> state_{
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    std::array<std::uint8_t, 64u> buffer_{};
    std::size_t buffer_size_ = 0u;
    std::uint64_t total_bytes_ = 0u;
};

[[nodiscard]] NativePortTexturePayloadSha256
decoded_texture_payload_sha256(const NativePortDecodedTextureAsset& texture) {
    // This is deliberately a byte-canonical stream instead of a hash of a
    // C++ object representation.  The metadata distinguishes otherwise equal
    // pixels from different source formats, extents, or archive positions;
    // the RGBA8 bytes are exactly the payload sent to the host texture.
    TexturePayloadSha256Accumulator accumulator;
    constexpr std::string_view identity_domain =
        "katana-native-texture-rgba8-v1";
    accumulator.update(identity_domain);

    const auto update_u32 = [&accumulator](const std::uint32_t value) {
        std::array<char, 4u> bytes{};
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] = static_cast<char>(value >> (index * 8u));
        accumulator.update(std::string_view(bytes.data(), bytes.size()));
    };
    const auto update_u64 = [&accumulator](const std::uint64_t value) {
        std::array<char, 8u> bytes{};
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] = static_cast<char>(value >> (index * 8u));
        accumulator.update(std::string_view(bytes.data(), bytes.size()));
    };
    const auto update_pixels = [&accumulator](
                                const std::vector<std::uint8_t>& pixels) {
        if (!pixels.empty())
            accumulator.update(std::string_view(
                reinterpret_cast<const char*>(pixels.data()), pixels.size()));
    };

    update_u32(static_cast<std::uint32_t>(texture.source_pixel_format));
    update_u32(static_cast<std::uint32_t>(texture.source_data_format));
    update_u32(texture.archive_ordinal);
    update_u32(texture.extent.width);
    update_u32(texture.extent.height);
    update_u32(static_cast<std::uint32_t>(texture.lower_mip_levels.size()));
    update_u64(static_cast<std::uint64_t>(texture.rgba8.size()));
    update_pixels(texture.rgba8);
    for (const auto& level : texture.lower_mip_levels) {
        update_u32(level.extent.width);
        update_u32(level.extent.height);
        update_u64(static_cast<std::uint64_t>(level.rgba8.size()));
        update_pixels(level.rgba8);
    }

    return accumulator.finish();
}

void bind_decoded_texture_payload_identity(
    NativePortDecodedTextureAsset& texture) {
    if (texture.decoded_payload_identity_bound) return;
    texture.decoded_rgba8_sha256 = decoded_texture_payload_sha256(texture);
    texture.decoded_payload_identity_bound = true;
}

[[nodiscard]] bool is_small_vector_quantized(
    const NativePortTextureAssetDataFormat format) noexcept {
    return format ==
               NativePortTextureAssetDataFormat::SmallVectorQuantized ||
           format == NativePortTextureAssetDataFormat::
                         SmallVectorQuantizedMipmaps;
}

[[nodiscard]] std::size_t small_vector_quantized_codebook_entries(
    const NativePortTextureAssetDataFormat format,
    const std::uint32_t width,
    const std::size_t offset) {
    if (format == NativePortTextureAssetDataFormat::SmallVectorQuantized) {
        if (width <= 16u) return 64u;
        if (width <= 32u) return 128u;
        if (width <= 64u) return 512u;
        return 1'024u;
    }
    if (format ==
        NativePortTextureAssetDataFormat::SmallVectorQuantizedMipmaps) {
        if (width <= 16u) return 64u;
        if (width <= 32u) return 256u;
        return 1'024u;
    }
    fail(NativePortTextureAssetFailure::InvalidPvrt, offset,
         "pvrt-small-vq-format");
}

[[nodiscard]] std::size_t mip_level_bytes(
    const NativePortTextureAssetDataFormat format,
    const std::uint32_t dimension,
    const std::size_t offset) {
    const auto pixels = checked_multiply(
        dimension, dimension, NativePortTextureAssetFailure::InvalidPvrt,
        offset, "pvrt-mipmap-pixels");
    if (is_vector_quantized(format))
        return std::max(pixels / 4u, std::size_t{1u});
    if (is_twiddled(format)) {
        // PVRT data-format 0x02 is the compact SDK file representation, not
        // the raw VRAM/0x12 layout.  It drops the first two 16-bit words from
        // OtherMipPoint, so the 1x1 level occupies four bytes and the 2x2
        // level starts at byte four.  Any remaining bytes in the PVRT chunk
        // are validated separately as writer alignment/trailer bytes.
        if (dimension == 1u) return 4u;
        return checked_multiply(
            pixels, 2u, NativePortTextureAssetFailure::InvalidPvrt, offset,
            "pvrt-mipmap-bytes");
    }
    fail(NativePortTextureAssetFailure::InvalidPvrt, offset,
         "pvrt-mipmap-format");
}

struct PvrDataLayout final {
    std::size_t encoded_bytes = 0u;
    std::size_t top_level_offset = 0u;
    std::size_t top_level_bytes = 0u;
    std::size_t codebook_entries = 0u;
    std::size_t codebook_bytes = 0u;
};

constexpr std::size_t full_vq_codebook_bytes = 2'048u;

[[nodiscard]] bool valid_pvrt_trailer_size(
    const std::size_t encoded_bytes,
    const std::size_t trailer_bytes) noexcept {
    // Katana SDK writers observed in the bound corpus use one of four
    // structural endings: no trailer, 16-byte alignment, 32-byte alignment,
    // or the 32-byte-aligned footprint plus one 8-byte writer record.  Bind
    // those exact classes instead of accepting arbitrary data merely because
    // it happens to fit inside a 63-byte window.
    if (trailer_bytes == 0u) return true;
    const auto chunk_bytes = encoded_bytes + 16u;
    const auto padding_16 = (16u - (chunk_bytes & 15u)) & 15u;
    const auto padding_32 = (32u - (chunk_bytes & 31u)) & 31u;
    return trailer_bytes == padding_16 || trailer_bytes == padding_32 ||
           trailer_bytes == padding_32 + 8u;
}

[[nodiscard]] PvrDataLayout pvr_data_layout(
    const NativePortExtent extent,
    const NativePortTextureAssetDataFormat format,
    const std::size_t offset) {
    PvrDataLayout layout;
    const auto pixels = checked_multiply(
        extent.width, extent.height,
        NativePortTextureAssetFailure::InvalidPvrt, offset,
        "pvrt-layout-pixels");
    if (format == NativePortTextureAssetDataFormat::Rectangle ||
        format == NativePortTextureAssetDataFormat::SquareTwiddled) {
        layout.top_level_bytes = checked_multiply(
            pixels, 2u, NativePortTextureAssetFailure::InvalidPvrt, offset,
            "pvrt-layout-bytes");
        layout.encoded_bytes = layout.top_level_bytes;
        return layout;
    }

    layout.top_level_bytes = mip_level_bytes(format, extent.width, offset);
    std::size_t lower_mipmap_bytes = 0u;
    if (has_mipmaps(format)) {
        for (std::uint32_t dimension = 1u; dimension < extent.width;
             dimension <<= 1u) {
            lower_mipmap_bytes = checked_add(
                lower_mipmap_bytes,
                mip_level_bytes(format, dimension, offset),
                NativePortTextureAssetFailure::InvalidPvrt, offset,
                "pvrt-mipmap-chain");
        }
    }

    if (is_vector_quantized(format)) {
        if (is_small_vector_quantized(format)) {
            layout.codebook_entries =
                small_vector_quantized_codebook_entries(format, extent.width,
                                                          offset);
        } else {
            layout.codebook_entries = 1'024u;
        }
        layout.codebook_bytes = checked_multiply(
            layout.codebook_entries, 2u,
            NativePortTextureAssetFailure::InvalidPvrt, offset,
            "pvrt-codebook-bytes");
    }

    layout.top_level_offset =
        checked_add(layout.codebook_bytes, lower_mipmap_bytes,
                    NativePortTextureAssetFailure::InvalidPvrt, offset,
                    "pvrt-top-level-offset");
    layout.encoded_bytes = checked_add(
        layout.top_level_offset, layout.top_level_bytes,
        NativePortTextureAssetFailure::InvalidPvrt, offset,
        "pvrt-layout-size");
    return layout;
}

[[nodiscard]] PvrDataLayout select_pvm_data_layout(
    const NativePortExtent extent,
    const NativePortTextureAssetDataFormat format,
    const std::size_t available_bytes,
    const std::size_t source_offset) {
    auto layout = pvr_data_layout(extent, format, source_offset);
    if (layout.encoded_bytes > available_bytes)
        fail(NativePortTextureAssetFailure::InvalidPvrt, source_offset,
             "pvrt-truncated-pixels");

    const auto compact_trailer = available_bytes - layout.encoded_bytes;
    if (is_small_vector_quantized(format)) {
        // Some SDK writers retain the unused footprint of a full 2-KiB
        // codebook *after* the compact codebook and index stream. It is
        // trailing reservation, not part of the physical index stream:
        // preserve the compact semantic codebook and offsets while accepting
        // only that bounded reservation plus chunk alignment.
        const auto unused_codebook_bytes =
            full_vq_codebook_bytes - layout.codebook_bytes;
        const auto compact_alignment = valid_pvrt_trailer_size(
            layout.encoded_bytes, compact_trailer);
        const auto full_footprint_reservation =
            compact_trailer >= unused_codebook_bytes &&
            valid_pvrt_trailer_size(
                checked_add(layout.encoded_bytes, unused_codebook_bytes,
                            NativePortTextureAssetFailure::InvalidPvrt,
                            source_offset, "pvrt-small-vq-reservation"),
                compact_trailer - unused_codebook_bytes);
        if (!compact_alignment && !full_footprint_reservation)
            fail(NativePortTextureAssetFailure::InvalidPvrt, source_offset,
                 "pvrt-small-vq-layout");
    } else if (!valid_pvrt_trailer_size(layout.encoded_bytes,
                                        compact_trailer)) {
        fail(NativePortTextureAssetFailure::InvalidPvrt, source_offset,
             "pvrt-padding");
    }
    return layout;
}

[[nodiscard]] std::uint32_t morton_index(const std::uint32_t x,
                                         const std::uint32_t y,
                                         const std::uint32_t dimension) {
    std::uint32_t result = 0u;
    for (std::uint32_t bit = 0u; (1u << bit) < dimension; ++bit) {
        result |= ((y >> bit) & 1u) << (bit * 2u);
        result |= ((x >> bit) & 1u) << (bit * 2u + 1u);
    }
    return result;
}

void decode_pixel(const std::uint16_t source,
                  const NativePortTextureAssetPixelFormat format,
                  std::uint8_t* const destination) noexcept {
    const auto expand4 = [](const std::uint16_t value) {
        return static_cast<std::uint8_t>((value << 4u) | value);
    };
    const auto expand5 = [](const std::uint16_t value) {
        return static_cast<std::uint8_t>((value << 3u) | (value >> 2u));
    };
    const auto expand6 = [](const std::uint16_t value) {
        return static_cast<std::uint8_t>((value << 2u) | (value >> 4u));
    };

    switch (format) {
    case NativePortTextureAssetPixelFormat::Argb1555:
        destination[0] = expand5((source >> 10u) & 0x1Fu);
        destination[1] = expand5((source >> 5u) & 0x1Fu);
        destination[2] = expand5(source & 0x1Fu);
        destination[3] = (source & 0x8000u) != 0u ? 255u : 0u;
        break;
    case NativePortTextureAssetPixelFormat::Rgb565:
        destination[0] = expand5((source >> 11u) & 0x1Fu);
        destination[1] = expand6((source >> 5u) & 0x3Fu);
        destination[2] = expand5(source & 0x1Fu);
        destination[3] = 255u;
        break;
    case NativePortTextureAssetPixelFormat::Argb4444:
        destination[0] = expand4((source >> 8u) & 0x0Fu);
        destination[1] = expand4((source >> 4u) & 0x0Fu);
        destination[2] = expand4(source & 0x0Fu);
        destination[3] = expand4((source >> 12u) & 0x0Fu);
        break;
    }
}

[[nodiscard]] std::size_t mip_level_offset(
    const PvrDataLayout& layout,
    NativePortTextureAssetDataFormat format,
    std::uint32_t dimension,
    std::size_t source_base_offset);

void decode_pixels(std::span<const std::uint8_t> source,
                   const PvrDataLayout& layout,
                   NativePortTextureAssetPixelFormat pixel_format,
                   NativePortTextureAssetDataFormat data_format,
                   NativePortExtent extent,
                   std::size_t level_offset,
                   std::vector<std::uint8_t>& rgba8,
                   std::size_t source_base_offset);

[[nodiscard]] NativePortDecodedTextureAsset decode_surface_impl(
    const std::span<const std::uint8_t> source,
    const NativePortExtent extent,
    const NativePortTextureAssetPixelFormat pixel_format,
    const NativePortTextureAssetDataFormat data_format,
    const NativePortTextureAssetLimits& limits) {
    validate_limits(limits);
    if (source.size() > limits.maximum_decompressed_bytes)
        fail(NativePortTextureAssetFailure::DecompressedOutputLimit, 0u,
             "surface-input-limit");
    switch (pixel_format) {
    case NativePortTextureAssetPixelFormat::Argb1555:
    case NativePortTextureAssetPixelFormat::Rgb565:
    case NativePortTextureAssetPixelFormat::Argb4444:
        break;
    default:
        fail(NativePortTextureAssetFailure::UnsupportedPixelFormat, 0u,
             "surface-pixel-format");
    }
    validate_dimensions(extent, data_format, limits, 0u);
    const auto layout = pvr_data_layout(extent, data_format, 0u);
    if (source.size() != layout.encoded_bytes)
        fail(NativePortTextureAssetFailure::InvalidPvrt, source.size(),
             "surface-encoded-size");

    const auto top_pixels = checked_multiply(
        extent.width, extent.height,
        NativePortTextureAssetFailure::RgbaOutputLimit, 0u,
        "surface-rgba-pixels");
    auto aggregate_rgba_bytes = checked_multiply(
        top_pixels, 4u, NativePortTextureAssetFailure::RgbaOutputLimit, 0u,
        "surface-rgba-bytes");
    if (has_mipmaps(data_format)) {
        for (auto dimension = extent.width / 2u;; dimension >>= 1u) {
            const auto level_bytes = checked_multiply(
                checked_multiply(
                    dimension, dimension,
                    NativePortTextureAssetFailure::RgbaOutputLimit, 0u,
                    "surface-mipmap-pixels"),
                4u, NativePortTextureAssetFailure::RgbaOutputLimit, 0u,
                "surface-mipmap-bytes");
            aggregate_rgba_bytes = checked_add(
                aggregate_rgba_bytes, level_bytes,
                NativePortTextureAssetFailure::RgbaOutputLimit, 0u,
                "surface-mipmap-chain");
            if (dimension == 1u) break;
        }
    }
    if (aggregate_rgba_bytes > limits.maximum_rgba_bytes)
        fail(NativePortTextureAssetFailure::RgbaOutputLimit, 0u,
             "surface-rgba-limit");

    NativePortDecodedTextureAsset texture;
    texture.source_pixel_format = pixel_format;
    texture.source_data_format = data_format;
    texture.extent = extent;
    texture.rgba8.resize(top_pixels * 4u);
    decode_pixels(source, layout, pixel_format, data_format, extent,
                  layout.top_level_offset, texture.rgba8, 0u);
    if (has_mipmaps(data_format)) {
        texture.lower_mip_levels.reserve(std::bit_width(extent.width) - 1u);
        for (auto dimension = extent.width / 2u;; dimension >>= 1u) {
            NativePortDecodedTextureMipLevel level;
            level.extent = {dimension, dimension};
            level.rgba8.resize(
                static_cast<std::size_t>(dimension) * dimension * 4u);
            decode_pixels(
                source, layout, pixel_format, data_format, level.extent,
                mip_level_offset(layout, data_format, dimension, 0u),
                level.rgba8, 0u);
            texture.lower_mip_levels.push_back(std::move(level));
            if (dimension == 1u) break;
        }
    }
    return texture;
}

[[nodiscard]] std::size_t mip_level_offset(
    const PvrDataLayout& layout,
    const NativePortTextureAssetDataFormat format,
    const std::uint32_t dimension,
    const std::size_t source_base_offset) {
    if (!has_mipmaps(format)) return layout.top_level_offset;
    if (is_vector_quantized(format) && dimension == 1u)
        return layout.codebook_bytes;
    // In compact PVRT data-format 0x02, the 1x1 texel is the second 16-bit
    // word.  Flycast expresses the same file-layout distinction as
    // (OtherMipPoint[TexU] - 2) * 2, while raw VRAM/format 0x12 uses the
    // unadjusted table.  This decoder accepts only the former format.
    if (is_twiddled(format) && dimension == 1u) return 2u;

    auto result = is_vector_quantized(format) ? layout.codebook_bytes : 0u;
    for (std::uint32_t lower = 1u; lower < dimension; lower <<= 1u)
        result = checked_add(
            result, mip_level_bytes(format, lower, source_base_offset),
            NativePortTextureAssetFailure::InvalidPvrt, source_base_offset,
            "pvrt-mipmap-offset");
    return result;
}

void decode_pixels(const std::span<const std::uint8_t> source,
                   const PvrDataLayout& layout,
                   const NativePortTextureAssetPixelFormat pixel_format,
                   const NativePortTextureAssetDataFormat data_format,
                   const NativePortExtent extent,
                   const std::size_t level_offset,
                   std::vector<std::uint8_t>& rgba8,
                   const std::size_t source_base_offset) {
    const auto width = extent.width;
    const auto height = extent.height;
    if (is_vector_quantized(data_format) && width == 1u) {
        if (level_offset >= source.size())
            fail(NativePortTextureAssetFailure::InvalidPvrt,
                 source_base_offset + level_offset, "pvrt-vq-1x1-index");
        const auto codebook_block = source[level_offset];
        const auto codebook_blocks = layout.codebook_entries / 4u;
        if (codebook_block >= codebook_blocks)
            fail(NativePortTextureAssetFailure::InvalidPvrt,
                 source_base_offset + level_offset, "pvrt-vq-1x1-index");
        // VQ mip chains store a distinct 1x1 index at codebook+0.  Decode its
        // 2x2 codebook vector and select the bottom-right texel, matching the
        // PowerVR mip layout.  The 2x2 level begins at codebook+1 and must not
        // be reused for this level.
        const auto codebook_offset =
            (static_cast<std::size_t>(codebook_block) * 4u + 3u) * 2u;
        const auto value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(source[codebook_offset]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(source[codebook_offset + 1u])
                << 8u));
        decode_pixel(value, pixel_format, rgba8.data());
        return;
    }

    const auto level_bytes =
        is_twiddled(data_format) && has_mipmaps(data_format) && width == 1u
            ? std::size_t{2u}
        : data_format == NativePortTextureAssetDataFormat::Rectangle
            ? checked_multiply(
                  checked_multiply(
                      width, height,
                      NativePortTextureAssetFailure::InvalidPvrt,
                      source_base_offset + level_offset,
                      "pvrt-rectangle-pixels"),
                  2u, NativePortTextureAssetFailure::InvalidPvrt,
                  source_base_offset + level_offset,
                  "pvrt-rectangle-bytes")
            : mip_level_bytes(data_format, width,
                              source_base_offset + level_offset);
    if (level_offset > source.size() ||
        level_bytes > source.size() - level_offset)
        fail(NativePortTextureAssetFailure::InvalidPvrt,
             source_base_offset + level_offset, "pvrt-mipmap-bytes");
    const auto level = source.subspan(level_offset, level_bytes);
    if (is_vector_quantized(data_format)) {
        const auto block_dimension = width / 2u;
        const auto codebook_blocks = layout.codebook_entries / 4u;
        for (std::uint32_t y = 0u; y < height; y += 2u) {
            for (std::uint32_t x = 0u; x < width; x += 2u) {
                const auto index_offset = static_cast<std::size_t>(
                    morton_index(x / 2u, y / 2u, block_dimension));
                const auto codebook_block = level[index_offset];
                if (codebook_block >= codebook_blocks)
                    fail(NativePortTextureAssetFailure::InvalidPvrt,
                         source_base_offset + level_offset + index_offset,
                         "pvrt-vq-index");
                for (std::uint32_t local_x = 0u; local_x < 2u; ++local_x) {
                    for (std::uint32_t local_y = 0u; local_y < 2u;
                         ++local_y) {
                        const auto codebook_entry =
                            static_cast<std::size_t>(codebook_block) * 4u +
                            local_x * 2u + local_y;
                        const auto codebook_offset = codebook_entry * 2u;
                        const std::uint16_t value = static_cast<std::uint16_t>(
                            static_cast<std::uint16_t>(
                                source[codebook_offset]) |
                            static_cast<std::uint16_t>(
                                static_cast<std::uint16_t>(
                                    source[codebook_offset + 1u])
                                << 8u));
                        const auto destination_offset =
                            (static_cast<std::size_t>(y + local_y) * width +
                             x + local_x) *
                            4u;
                        decode_pixel(value, pixel_format,
                                     rgba8.data() + destination_offset);
                    }
                }
            }
        }
        return;
    }

    for (std::uint32_t y = 0u; y < height; ++y) {
        for (std::uint32_t x = 0u; x < width; ++x) {
            const std::size_t source_pixel =
                is_twiddled(data_format)
                    ? morton_index(x, y, width)
                    : static_cast<std::size_t>(y) * width + x;
            const std::size_t source_offset = source_pixel * 2u;
            const std::uint16_t value =
                static_cast<std::uint16_t>(level[source_offset]) |
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(level[source_offset + 1u])
                    << 8u);
            const std::size_t destination_offset =
                (static_cast<std::size_t>(y) * width + x) * 4u;
            decode_pixel(value, pixel_format,
                         rgba8.data() + destination_offset);
        }
    }
}

[[nodiscard]] std::vector<NativePortDecodedTextureAsset> decode_pvm_impl(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits,
    const std::optional<std::uint32_t> captured_ordinal = std::nullopt,
    NativePortEncodedTextureAsset* const captured_texture = nullptr,
    const bool decode_rgba = true) {
    if (captured_ordinal.has_value() != (captured_texture != nullptr))
        fail(NativePortTextureAssetFailure::InvalidLimits, 0u,
             "pvm-capture-contract");
    validate_limits(limits);
    if (source.size() > limits.maximum_decompressed_bytes)
        fail(NativePortTextureAssetFailure::DecompressedOutputLimit, 0u,
             "pvm-input-limit");
    if (source.size() < 12u ||
        read_u32(source, 0u, NativePortTextureAssetFailure::InvalidPvm,
                 "pvm-header") != pvm_magic)
        fail(NativePortTextureAssetFailure::InvalidPvm, 0u, "pvm-magic");

    const auto header_payload_size = read_u32(
        source, 4u, NativePortTextureAssetFailure::InvalidPvm,
        "pvm-header-size");
    const auto first_texture_offset = checked_add(
        8u, header_payload_size, NativePortTextureAssetFailure::InvalidPvm,
        4u, "pvm-header-size");
    if (first_texture_offset < 12u || first_texture_offset > source.size())
        fail(NativePortTextureAssetFailure::InvalidPvm, 4u,
             "pvm-header-boundary");

    const auto flags = read_u16(
        source, 8u, NativePortTextureAssetFailure::InvalidPvm, "pvm-flags");
    if ((flags & ~supported_pvm_flags) != 0u ||
        (flags & pvm_pvrt_flag) == 0u)
        fail(NativePortTextureAssetFailure::UnsupportedPvmFeature, 8u,
             "pvm-flags");
    const auto entry_count = read_u16(
        source, 10u, NativePortTextureAssetFailure::InvalidPvm,
        "pvm-entry-count");
    if (entry_count > limits.maximum_archive_entries)
        fail(NativePortTextureAssetFailure::ArchiveEntryLimit, 10u,
             "pvm-entry-limit");
    if (captured_ordinal.has_value() && *captured_ordinal >= entry_count)
        fail(NativePortTextureAssetFailure::InvalidPvm, 10u,
             "pvm-entry-ordinal-range");

    std::size_t entry_size = 2u;
    if ((flags & pvm_filename_flag) != 0u)
        entry_size = checked_add(
            entry_size, pvm_name_bytes,
            NativePortTextureAssetFailure::InvalidPvm, 8u,
            "pvm-entry-size");
    if ((flags & pvm_pixel_data_format_flag) != 0u)
        entry_size = checked_add(
            entry_size, 2u, NativePortTextureAssetFailure::InvalidPvm, 8u,
            "pvm-entry-size");
    if ((flags & pvm_texture_dimensions_flag) != 0u)
        entry_size = checked_add(
            entry_size, 2u, NativePortTextureAssetFailure::InvalidPvm, 8u,
            "pvm-entry-size");
    if ((flags & pvm_global_index_flag) != 0u)
        entry_size = checked_add(
            entry_size, 4u, NativePortTextureAssetFailure::InvalidPvm, 8u,
            "pvm-entry-size");

    const auto table_size = checked_multiply(
        entry_size, entry_count, NativePortTextureAssetFailure::InvalidPvm,
        10u, "pvm-entry-table-size");
    const auto table_end = checked_add(
        12u, table_size, NativePortTextureAssetFailure::InvalidPvm, 10u,
        "pvm-entry-table-size");
    if (table_end > first_texture_offset ||
        !all_zero(source.subspan(table_end, first_texture_offset - table_end)))
        fail(NativePortTextureAssetFailure::InvalidPvm, table_end,
             "pvm-header-padding");

    std::vector<PvmEntryMetadata> metadata;
    metadata.reserve(entry_count);
    for (std::size_t index = 0u; index < entry_count; ++index) {
        std::size_t cursor = 12u + index * entry_size;
        const auto ordinal = read_u16(
            source, cursor, NativePortTextureAssetFailure::InvalidPvm,
            "pvm-entry-ordinal");
        if (ordinal != static_cast<std::uint16_t>(index))
            fail(NativePortTextureAssetFailure::InvalidPvm, cursor,
                 "pvm-entry-ordinal");
        cursor += 2u;
        PvmEntryMetadata entry;
        if ((flags & pvm_filename_flag) != 0u) {
            const auto name_bytes = source.subspan(cursor, pvm_name_bytes);
            const auto terminator = std::ranges::find(name_bytes, 0u);
            entry.name.assign(
                reinterpret_cast<const char*>(name_bytes.data()),
                static_cast<std::size_t>(terminator - name_bytes.begin()));
            cursor += pvm_name_bytes;
        }
        if ((flags & pvm_pixel_data_format_flag) != 0u) {
            entry.pixel_format = source[cursor++];
            entry.data_format = source[cursor++];
        }
        if ((flags & pvm_texture_dimensions_flag) != 0u) {
            entry.dimensions = read_u16(
                source, cursor, NativePortTextureAssetFailure::InvalidPvm,
                "pvm-entry-dimensions");
            cursor += 2u;
        }
        if ((flags & pvm_global_index_flag) != 0u) {
            entry.global_index = read_u32(
                source, cursor, NativePortTextureAssetFailure::InvalidPvm,
                "pvm-entry-global-index");
        }
        metadata.push_back(std::move(entry));
    }

    std::vector<NativePortDecodedTextureAsset> textures;
    textures.reserve(entry_count);
    std::size_t aggregate_rgba_bytes = 0u;
    std::size_t chunk_offset = first_texture_offset;
    for (std::size_t index = 0u; index < entry_count; ++index) {
        if (chunk_offset > source.size() || source.size() - chunk_offset < 16u)
            fail(NativePortTextureAssetFailure::InvalidPvrt, chunk_offset,
                 "pvrt-header");
        if (read_u32(source, chunk_offset,
                     NativePortTextureAssetFailure::InvalidPvrt,
                     "pvrt-magic") != pvrt_magic)
            fail(NativePortTextureAssetFailure::InvalidPvrt, chunk_offset,
                 "pvrt-magic");
        const auto chunk_payload_size = read_u32(
            source, chunk_offset + 4u,
            NativePortTextureAssetFailure::InvalidPvrt, "pvrt-chunk-size");
        if (chunk_payload_size < 8u)
            fail(NativePortTextureAssetFailure::InvalidPvrt,
                 chunk_offset + 4u, "pvrt-chunk-size");
        const auto chunk_size = checked_add(
            8u, chunk_payload_size,
            NativePortTextureAssetFailure::InvalidPvrt, chunk_offset + 4u,
            "pvrt-chunk-size");
        const auto chunk_end = checked_add(
            chunk_offset, chunk_size,
            NativePortTextureAssetFailure::InvalidPvrt, chunk_offset + 4u,
            "pvrt-chunk-boundary");
        if (chunk_end > source.size())
            fail(NativePortTextureAssetFailure::InvalidPvrt,
                 chunk_offset + 4u, "pvrt-chunk-boundary");

        NativePortDecodedTextureAsset texture;
        texture.name = metadata[index].name;
        texture.global_index = metadata[index].global_index;
        texture.archive_ordinal = static_cast<std::uint32_t>(index);
        texture.source_pixel_format =
            parse_pixel_format(source[chunk_offset + 8u], chunk_offset + 8u);
        texture.source_data_format =
            parse_data_format(source[chunk_offset + 9u], chunk_offset + 9u);
        texture.extent.width = read_u16(
            source, chunk_offset + 12u,
            NativePortTextureAssetFailure::InvalidPvrt, "pvrt-width");
        texture.extent.height = read_u16(
            source, chunk_offset + 14u,
            NativePortTextureAssetFailure::InvalidPvrt, "pvrt-height");
        validate_dimensions(texture.extent, texture.source_data_format, limits,
                            chunk_offset + 12u);

        if ((metadata[index].pixel_format.has_value() &&
             *metadata[index].pixel_format != source[chunk_offset + 8u]) ||
            (metadata[index].data_format.has_value() &&
             *metadata[index].data_format != source[chunk_offset + 9u]))
            fail(NativePortTextureAssetFailure::InvalidPvm,
                 12u + index * entry_size, "pvm-pvrt-format-mismatch");
        if (metadata[index].dimensions.has_value()) {
            const auto dimensions = *metadata[index].dimensions;
            const auto encoded_width =
                static_cast<std::uint32_t>(1u)
                << ((dimensions & 0x0Fu) + 2u);
            const auto encoded_height =
                static_cast<std::uint32_t>(1u)
                << (((dimensions >> 4u) & 0x0Fu) + 2u);
            if (encoded_width != texture.extent.width ||
                encoded_height != texture.extent.height)
                fail(NativePortTextureAssetFailure::InvalidPvm,
                     12u + index * entry_size,
                     "pvm-pvrt-dimension-mismatch");
        }

        const auto pixel_count = checked_multiply(
            texture.extent.width, texture.extent.height,
            NativePortTextureAssetFailure::InvalidDimensions,
            chunk_offset + 12u, "pvrt-pixel-count");
        const auto rgba_bytes = checked_multiply(
            pixel_count, 4u,
            NativePortTextureAssetFailure::RgbaOutputLimit,
            chunk_offset + 12u, "pvrt-rgba-bytes");
        const auto available_data_bytes = chunk_payload_size - 8u;
        const auto pixel_data_offset = chunk_offset + 16u;
        const auto layout = select_pvm_data_layout(
            texture.extent, texture.source_data_format,
            available_data_bytes, chunk_offset + 9u);
        const auto encoded_bytes = layout.encoded_bytes;
        // select_pvm_data_layout has already validated the bounded trailing
        // alignment/reservation without changing the compact index offset.
        auto decoded_rgba_bytes = rgba_bytes;
        if (has_mipmaps(texture.source_data_format)) {
            for (auto dimension = texture.extent.width / 2u;;
                 dimension >>= 1u) {
                const auto level_pixels = checked_multiply(
                    dimension, dimension,
                    NativePortTextureAssetFailure::RgbaOutputLimit,
                    chunk_offset + 12u, "pvrt-mipmap-rgba-pixels");
                const auto level_rgba_bytes = checked_multiply(
                    level_pixels, 4u,
                    NativePortTextureAssetFailure::RgbaOutputLimit,
                    chunk_offset + 12u, "pvrt-mipmap-rgba-bytes");
                decoded_rgba_bytes = checked_add(
                    decoded_rgba_bytes, level_rgba_bytes,
                    NativePortTextureAssetFailure::RgbaOutputLimit,
                    chunk_offset + 12u, "pvrt-mipmap-rgba-chain");
                if (dimension == 1u) break;
            }
        }
        aggregate_rgba_bytes = checked_add(
            aggregate_rgba_bytes, decoded_rgba_bytes,
            NativePortTextureAssetFailure::RgbaOutputLimit, chunk_offset,
            "pvm-rgba-size");
        if (aggregate_rgba_bytes > limits.maximum_rgba_bytes)
            fail(NativePortTextureAssetFailure::RgbaOutputLimit,
                 chunk_offset, "pvm-rgba-limit");

        const auto encoded = source.subspan(pixel_data_offset, encoded_bytes);
        if (captured_ordinal.has_value() &&
            *captured_ordinal == static_cast<std::uint32_t>(index)) {
            captured_texture->name = texture.name;
            captured_texture->global_index = texture.global_index;
            captured_texture->archive_ordinal = texture.archive_ordinal;
            captured_texture->source_pixel_format =
                texture.source_pixel_format;
            captured_texture->source_data_format =
                texture.source_data_format;
            captured_texture->extent = texture.extent;
            captured_texture->encoded.assign(encoded.begin(), encoded.end());
        }
        if (decode_rgba) {
            texture.rgba8.resize(rgba_bytes);
            decode_pixels(encoded, layout, texture.source_pixel_format,
                          texture.source_data_format, texture.extent,
                          layout.top_level_offset, texture.rgba8,
                          pixel_data_offset);
            if (has_mipmaps(texture.source_data_format)) {
                texture.lower_mip_levels.reserve(
                    std::bit_width(texture.extent.width) - 1u);
                for (auto dimension = texture.extent.width / 2u;;
                     dimension >>= 1u) {
                    NativePortDecodedTextureMipLevel level;
                    level.extent = {dimension, dimension};
                    level.rgba8.resize(
                        checked_multiply(
                            checked_multiply(
                                dimension, dimension,
                                NativePortTextureAssetFailure::
                                    RgbaOutputLimit,
                                chunk_offset + 12u,
                                "pvrt-mipmap-output-pixels"),
                            4u,
                            NativePortTextureAssetFailure::RgbaOutputLimit,
                            chunk_offset + 12u,
                            "pvrt-mipmap-output-bytes"));
                    decode_pixels(
                        encoded, layout, texture.source_pixel_format,
                        texture.source_data_format, level.extent,
                        mip_level_offset(layout, texture.source_data_format,
                                         dimension, pixel_data_offset),
                        level.rgba8, pixel_data_offset);
                    texture.lower_mip_levels.push_back(std::move(level));
                    if (dimension == 1u) break;
                }
            }
            bind_decoded_texture_payload_identity(texture);
            textures.push_back(std::move(texture));
        }
        chunk_offset = chunk_end;
    }

    if (chunk_offset != source.size())
        fail(NativePortTextureAssetFailure::InvalidPvm, chunk_offset,
             "pvm-trailing-data");
    if (captured_texture != nullptr && captured_texture->encoded.empty())
        fail(NativePortTextureAssetFailure::InvalidPvm, chunk_offset,
             "pvm-entry-capture");
    return textures;
}

[[nodiscard]] NativePortDecodedTextureAsset decode_pvr_impl(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
    validate_limits(limits);
    if (source.size() > limits.maximum_decompressed_bytes)
        fail(NativePortTextureAssetFailure::DecompressedOutputLimit, 0u,
             "pvr-input-limit");

    std::size_t chunk_offset = 0u;
    std::optional<std::uint32_t> global_index;
    if (source.size() >= 8u &&
        read_u32(source, 0u, NativePortTextureAssetFailure::InvalidPvrt,
                 "pvr-leading-chunk") == gbix_magic) {
        const auto payload_size = read_u32(
            source, 4u, NativePortTextureAssetFailure::InvalidPvrt,
            "gbix-chunk-size");
        if (payload_size < 4u)
            fail(NativePortTextureAssetFailure::InvalidPvrt, 4u,
                 "gbix-chunk-size");
        chunk_offset = checked_add(
            8u, payload_size, NativePortTextureAssetFailure::InvalidPvrt, 4u,
            "gbix-chunk-boundary");
        if (chunk_offset > source.size())
            fail(NativePortTextureAssetFailure::InvalidPvrt, 4u,
                 "gbix-chunk-boundary");
        global_index = read_u32(
            source, 8u, NativePortTextureAssetFailure::InvalidPvrt,
            "gbix-global-index");
    }

    if (chunk_offset > source.size() || source.size() - chunk_offset < 16u ||
        read_u32(source, chunk_offset,
                 NativePortTextureAssetFailure::InvalidPvrt,
                 "pvrt-magic") != pvrt_magic)
        fail(NativePortTextureAssetFailure::InvalidPvrt, chunk_offset,
             "pvrt-magic");
    const auto payload_size = read_u32(
        source, chunk_offset + 4u,
        NativePortTextureAssetFailure::InvalidPvrt, "pvrt-chunk-size");
    if (payload_size < 8u)
        fail(NativePortTextureAssetFailure::InvalidPvrt, chunk_offset + 4u,
             "pvrt-chunk-size");
    const auto chunk_size = checked_add(
        8u, payload_size, NativePortTextureAssetFailure::InvalidPvrt,
        chunk_offset + 4u, "pvrt-chunk-size");
    const auto chunk_end = checked_add(
        chunk_offset, chunk_size,
        NativePortTextureAssetFailure::InvalidPvrt, chunk_offset + 4u,
        "pvrt-chunk-boundary");
    if (chunk_end != source.size())
        fail(NativePortTextureAssetFailure::InvalidPvrt, chunk_end,
             "pvr-trailing-data");

    const auto pixel_format =
        parse_pixel_format(source[chunk_offset + 8u], chunk_offset + 8u);
    const auto data_format =
        parse_data_format(source[chunk_offset + 9u], chunk_offset + 9u);
    const NativePortExtent extent{
        read_u16(source, chunk_offset + 12u,
                 NativePortTextureAssetFailure::InvalidPvrt, "pvrt-width"),
        read_u16(source, chunk_offset + 14u,
                 NativePortTextureAssetFailure::InvalidPvrt, "pvrt-height")};
    validate_dimensions(extent, data_format, limits, chunk_offset + 12u);
    const auto available_data_bytes = payload_size - 8u;
    const auto layout = select_pvm_data_layout(
        extent, data_format, available_data_bytes, chunk_offset + 9u);
    const auto pixel_data_offset = chunk_offset + 16u;
    if (layout.encoded_bytes > source.size() - pixel_data_offset)
        fail(NativePortTextureAssetFailure::InvalidPvrt, pixel_data_offset,
             "pvrt-truncated-pixels");
    auto texture = decode_surface_impl(
        source.subspan(pixel_data_offset, layout.encoded_bytes), extent,
        pixel_format, data_format, limits);
    texture.global_index = global_index;
    texture.archive_ordinal = 0u;
    bind_decoded_texture_payload_identity(texture);
    return texture;
}

template <typename Function>
[[nodiscard]] auto translate_resource_failures(
    Function&& function,
    const std::string_view operation) -> decltype(function()) {
    try {
        return function();
    } catch (const NativePortTextureAssetError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw NativePortTextureAssetError(
            NativePortTextureAssetFailure::ResourceExhausted, 0u, operation);
    } catch (const std::length_error&) {
        throw NativePortTextureAssetError(
            NativePortTextureAssetFailure::ResourceExhausted, 0u, operation);
    }
}

} // namespace

class NativePortTextureBackingStore::Impl final {
  public:
    explicit Impl(const std::size_t capacity_bytes) {
        constexpr std::size_t maximum_capacity = 1u << 30u;
        if (capacity_bytes == 0u || capacity_bytes > maximum_capacity)
            backing_fail(
                NativePortTextureBackingStoreFailure::InvalidCapacity, 0u,
                "capacity");
        try {
            bytes_.resize(capacity_bytes);
            valid_words_.assign((capacity_bytes + 63u) / 64u, 0u);
        } catch (const std::bad_alloc&) {
            backing_fail(
                NativePortTextureBackingStoreFailure::ResourceExhausted, 0u,
                "allocation");
        } catch (const std::length_error&) {
            backing_fail(
                NativePortTextureBackingStoreFailure::ResourceExhausted, 0u,
                "allocation");
        }
    }

    void write(const std::uint32_t destination,
               const std::span<const std::uint8_t> source) {
        validate_range(destination, source.size(), "write");
        std::memcpy(bytes_.data() + destination, source.data(), source.size());
        for (std::size_t index = destination;
             index < static_cast<std::size_t>(destination) + source.size();
             ++index) {
            const auto word = index / 64u;
            const auto mask = std::uint64_t{1u} << (index % 64u);
            if ((valid_words_[word] & mask) != 0u) continue;
            valid_words_[word] |= mask;
            ++valid_bytes_;
        }
        if (writes_ != std::numeric_limits<std::uint64_t>::max()) ++writes_;
    }

    [[nodiscard]] NativePortTextureOverlayCoverage overlay(
        const std::uint32_t source_offset,
        const std::span<std::uint8_t> destination) const {
        validate_range(source_offset, destination.size(), "overlay");
        std::size_t copied = 0u;
        for (std::size_t index = 0u; index < destination.size(); ++index) {
            const auto source_index =
                static_cast<std::size_t>(source_offset) + index;
            const auto mask = std::uint64_t{1u} << (source_index % 64u);
            if ((valid_words_[source_index / 64u] & mask) == 0u) continue;
            destination[index] = bytes_[source_index];
            ++copied;
        }
        if (copied == 0u) return NativePortTextureOverlayCoverage::None;
        return copied == destination.size()
                   ? NativePortTextureOverlayCoverage::Complete
                   : NativePortTextureOverlayCoverage::Partial;
    }

    void clear() noexcept {
        std::ranges::fill(valid_words_, 0u);
        valid_bytes_ = 0u;
        writes_ = 0u;
    }

    [[nodiscard]] NativePortTextureBackingStoreSnapshot snapshot() const
        noexcept {
        return {bytes_.size(), valid_bytes_, writes_};
    }

  private:
    [[noreturn]] static void backing_fail(
        const NativePortTextureBackingStoreFailure failure,
        const std::uint64_t byte_offset,
        const std::string_view operation) {
        throw NativePortTextureBackingStoreError(failure, byte_offset,
                                                 operation);
    }

    void validate_range(const std::uint32_t offset,
                        const std::size_t bytes,
                        const std::string_view operation) const {
        if (bytes == 0u || offset >= bytes_.size() ||
            bytes > bytes_.size() - offset)
            backing_fail(NativePortTextureBackingStoreFailure::InvalidRange,
                         offset, operation);
    }

    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint64_t> valid_words_;
    std::uint64_t valid_bytes_ = 0u;
    std::uint64_t writes_ = 0u;
};

class NativePortTextureRegistry::Impl final {
  public:
    Impl(NativePortGraphicsDevice& graphics,
         const NativePortTextureRegistryLimits limits)
        : graphics_(graphics), limits_(limits) {
        if (limits.maximum_entries == 0u ||
            limits.maximum_entries > 1'048'576u ||
            limits.maximum_texture_bytes < 4u ||
            limits.maximum_texture_bytes >
                16ull * 1024u * 1024u * 1024u)
            registry_fail(NativePortTextureRegistryFailure::InvalidConfig, 0u,
                          "config");
        try {
            entries_.reserve(limits.maximum_entries);
            free_slots_.reserve(limits.maximum_entries);
            identity_index_.reserve(limits.maximum_entries);
            token_index_.reserve(limits.maximum_entries);
        } catch (const std::bad_alloc&) {
            registry_fail(NativePortTextureRegistryFailure::ResourceExhausted,
                          0u, "reserve");
        } catch (const std::length_error&) {
            registry_fail(NativePortTextureRegistryFailure::ResourceExhausted,
                          0u, "reserve");
        }
    }

    ~Impl() {
        try {
            invalidate_all();
        } catch (...) {
            // NativePortGraphicsDevice owns every remaining slot and releases
            // it during its own destruction. Explicit invalidation is the
            // error-reporting path.
        }
    }

    [[nodiscard]] NativePortTextureRegistryBinding acquire(
        const NativePortTextureAssetIdentity& identity,
        const NativePortDecodedTextureAsset& texture) {
        validate_identity(identity);
        if (identity.global_index != texture.global_index ||
            identity.archive_ordinal != texture.archive_ordinal)
            registry_fail(NativePortTextureRegistryFailure::InvalidIdentity,
                          0u, "identity-global-index");
        const auto texture_bytes = validate_texture(texture);

        const auto existing_index = identity_index_.find(identity);
        if (existing_index != identity_index_.end()) {
            auto& existing = entry_at(existing_index->second);
            if (existing.extent.width != texture.extent.width ||
                existing.extent.height != texture.extent.height ||
                existing.pixel_format != texture.source_pixel_format ||
                existing.data_format != texture.source_data_format ||
                existing.mip_levels !=
                    texture.lower_mip_levels.size() + 1u ||
                existing.texture_bytes != texture_bytes)
                registry_fail(
                    NativePortTextureRegistryFailure::IdentityCollision,
                    existing.guest_token, "identity-collision");
            if (existing.references ==
                std::numeric_limits<std::uint32_t>::max())
                registry_fail(
                    NativePortTextureRegistryFailure::ReferenceCountLimit,
                    existing.guest_token, "reference-count");
            if (acquired_references_ ==
                std::numeric_limits<std::uint64_t>::max())
                registry_fail(
                    NativePortTextureRegistryFailure::ReferenceCountLimit,
                    existing.guest_token, "aggregate-reference-count");
            ++existing.references;
            ++acquired_references_;
            return {existing.guest_token, existing.handle};
        }

        if (active_entries_ == limits_.maximum_entries)
            registry_fail(NativePortTextureRegistryFailure::EntryLimit, 0u,
                          "entry-limit");
        if (texture_bytes > limits_.maximum_texture_bytes - texture_bytes_)
            registry_fail(NativePortTextureRegistryFailure::ByteLimit, 0u,
                          "byte-limit");
        if (next_guest_token_ == 0u)
            registry_fail(NativePortTextureRegistryFailure::TokenExhausted,
                          0u, "token-exhausted");

        const auto decoded_payload_sha256 =
            texture.decoded_payload_identity_bound
                ? texture.decoded_rgba8_sha256
                : decoded_texture_payload_sha256(texture);
        NativePortTextureConfig config;
        config.extent = texture.extent;
        config.format = NativePortTextureFormat::Rgba8Unorm;
        config.mip_levels = static_cast<std::uint32_t>(
            texture.lower_mip_levels.size() + 1u);
        config.provenance.content_sha256 = identity.content_sha256;
        config.provenance.generation = identity.generation;
        config.provenance.archive_ordinal = identity.archive_ordinal;
        config.provenance.content_identity_bound = true;
        config.provenance.global_index = identity.global_index.value_or(0u);
        config.provenance.global_index_bound = identity.global_index.has_value();
        config.provenance.decoded_rgba8_sha256 = decoded_payload_sha256;
        config.provenance.source_pixel_format = static_cast<std::uint8_t>(
            texture.source_pixel_format);
        config.provenance.source_data_format = static_cast<std::uint8_t>(
            texture.source_data_format);
        config.provenance.decoded_extent = texture.extent;
        config.provenance.decoded_mip_levels = config.mip_levels;
        config.provenance.decoded_payload_identity_bound = true;
        std::vector<NativePortImageView> images;
        images.reserve(config.mip_levels);
        images.push_back(NativePortImageView{
            texture.extent,
            NativePortTextureFormat::Rgba8Unorm,
            texture.extent.width * 4u,
            false,
            std::as_bytes(std::span(texture.rgba8))});
        for (const auto& level : texture.lower_mip_levels)
            images.push_back(NativePortImageView{
                level.extent,
                NativePortTextureFormat::Rgba8Unorm,
                level.extent.width * 4u,
                false,
                std::as_bytes(std::span(level.rgba8))});
        const auto handle = graphics_.create_texture(config, images);
        const auto guest_token = next_guest_token_;
        std::size_t entry_index = 0u;
        const bool reused_slot = !free_slots_.empty();
        try {
            if (reused_slot) {
                entry_index = free_slots_.back();
                free_slots_.pop_back();
                entries_[entry_index].emplace(
                    Entry{identity, guest_token, handle, 1u, texture_bytes,
                          texture.extent, texture.source_pixel_format,
                          texture.source_data_format, config.mip_levels});
            } else {
                entry_index = entries_.size();
                entries_.emplace_back(
                    std::in_place,
                    Entry{identity, guest_token, handle, 1u, texture_bytes,
                          texture.extent, texture.source_pixel_format,
                          texture.source_data_format, config.mip_levels});
            }
            const auto [identity_entry, identity_inserted] =
                identity_index_.emplace(identity, entry_index);
            if (!identity_inserted)
                registry_fail(
                    NativePortTextureRegistryFailure::IdentityCollision,
                    guest_token, "identity-index");
            try {
                const auto [token_entry, token_inserted] =
                    token_index_.emplace(guest_token, entry_index);
                if (!token_inserted)
                    registry_fail(
                        NativePortTextureRegistryFailure::TokenExhausted,
                        guest_token, "token-index");
                static_cast<void>(token_entry);
            } catch (...) {
                identity_index_.erase(identity_entry);
                throw;
            }
        } catch (...) {
            if (reused_slot) {
                if (entry_index < entries_.size())
                    entries_[entry_index].reset();
                free_slots_.push_back(entry_index);
            } else if (entry_index < entries_.size() &&
                       entries_[entry_index]) {
                entries_[entry_index].reset();
                if (entry_index + 1u == entries_.size())
                    entries_.pop_back();
            }
            graphics_.destroy_texture(handle);
            throw;
        }
        ++next_guest_token_;
        ++active_entries_;
        texture_bytes_ += texture_bytes;
        ++acquired_references_;
        return {guest_token, handle};
    }

    [[nodiscard]] NativePortTextureHandle resolve(
        const std::uint32_t guest_token,
        const std::uint64_t expected_generation) const {
        const auto& entry = entry_at(find_token_index(guest_token));
        if (entry.identity.generation != expected_generation)
            registry_fail(NativePortTextureRegistryFailure::GenerationMismatch,
                          guest_token, "resolve-generation");
        return entry.handle;
    }

    void release(const std::uint32_t guest_token,
                 const std::uint64_t expected_generation) {
        const auto entry_index = find_token_index(guest_token);
        auto& entry = entry_at(entry_index);
        if (entry.identity.generation != expected_generation)
            registry_fail(NativePortTextureRegistryFailure::GenerationMismatch,
                          guest_token, "release-generation");
        if (entry.references > 1u) {
            --entry.references;
            --acquired_references_;
            return;
        }
        graphics_.destroy_texture(entry.handle);
        texture_bytes_ -= entry.texture_bytes;
        --acquired_references_;
        erase_entry(entry_index);
    }

    void invalidate_generation(const std::uint64_t generation) {
        if (generation == 0u)
            registry_fail(NativePortTextureRegistryFailure::InvalidIdentity,
                          0u, "invalidate-generation");
        for (std::size_t index = 0u; index < entries_.size(); ++index) {
            if (!entries_[index] ||
                entries_[index]->identity.generation != generation)
                continue;
            graphics_.destroy_texture(entries_[index]->handle);
            texture_bytes_ -= entries_[index]->texture_bytes;
            acquired_references_ -= entries_[index]->references;
            erase_entry(index);
        }
    }

    void invalidate_all() {
        for (std::size_t index = 0u; index < entries_.size(); ++index) {
            if (!entries_[index]) continue;
            graphics_.destroy_texture(entries_[index]->handle);
            texture_bytes_ -= entries_[index]->texture_bytes;
            acquired_references_ -= entries_[index]->references;
            erase_entry(index);
        }
    }

    [[nodiscard]] NativePortTextureRegistrySnapshot snapshot() const noexcept {
        return {active_entries_, texture_bytes_,
                acquired_references_};
    }

  private:
    struct IdentityHash final {
        [[nodiscard]] std::size_t operator()(
            const NativePortTextureAssetIdentity& identity) const noexcept {
            std::uint64_t hash = 14'695'981'039'346'656'037ull;
            const auto mix = [&](const std::uint8_t byte) {
                hash ^= byte;
                hash *= 1'099'511'628'211ull;
            };
            const auto mix_integer = [&](const std::uint64_t value) {
                for (std::uint32_t shift = 0u; shift < 64u; shift += 8u)
                    mix(static_cast<std::uint8_t>(value >> shift));
            };
            mix_integer(identity.generation);
            mix(identity.global_index.has_value() ? 1u : 0u);
            mix_integer(identity.global_index.value_or(0u));
            mix_integer(identity.archive_ordinal);
            for (const auto byte : identity.content_sha256) mix(byte);
            if constexpr (sizeof(std::size_t) < sizeof(hash))
                return static_cast<std::size_t>(hash ^ (hash >> 32u));
            return static_cast<std::size_t>(hash);
        }
    };

    struct Entry final {
        NativePortTextureAssetIdentity identity;
        std::uint32_t guest_token;
        NativePortTextureHandle handle;
        std::uint32_t references;
        std::uint64_t texture_bytes;
        NativePortExtent extent;
        NativePortTextureAssetPixelFormat pixel_format;
        NativePortTextureAssetDataFormat data_format;
        std::uint32_t mip_levels;
    };

    [[noreturn]] static void registry_fail(
        const NativePortTextureRegistryFailure failure,
        const std::uint32_t guest_token,
        const std::string_view operation) {
        throw NativePortTextureRegistryError(failure, guest_token, operation);
    }

    static void validate_identity(
        const NativePortTextureAssetIdentity& identity) {
        if (identity.generation == 0u ||
            std::ranges::all_of(identity.content_sha256,
                                [](const std::uint8_t value) {
                                    return value == 0u;
                                }))
            registry_fail(NativePortTextureRegistryFailure::InvalidIdentity,
                          0u, "identity");
    }

    [[nodiscard]] static std::uint64_t validate_texture(
        const NativePortDecodedTextureAsset& texture) {
        if (texture.extent.width == 0u || texture.extent.height == 0u ||
            texture.extent.width > maximum_pvr_dimension ||
            texture.extent.height > maximum_pvr_dimension)
            registry_fail(NativePortTextureRegistryFailure::InvalidTexture, 0u,
                          "texture-extent");
        switch (texture.source_pixel_format) {
        case NativePortTextureAssetPixelFormat::Argb1555:
        case NativePortTextureAssetPixelFormat::Rgb565:
        case NativePortTextureAssetPixelFormat::Argb4444:
            break;
        default:
            registry_fail(NativePortTextureRegistryFailure::InvalidTexture, 0u,
                          "texture-pixel-format");
        }
        switch (texture.source_data_format) {
        case NativePortTextureAssetDataFormat::SquareTwiddled:
        case NativePortTextureAssetDataFormat::SquareTwiddledMipmaps:
        case NativePortTextureAssetDataFormat::VectorQuantized:
        case NativePortTextureAssetDataFormat::VectorQuantizedMipmaps:
        case NativePortTextureAssetDataFormat::Rectangle:
        case NativePortTextureAssetDataFormat::SmallVectorQuantized:
        case NativePortTextureAssetDataFormat::SmallVectorQuantizedMipmaps:
            break;
        default:
            registry_fail(NativePortTextureRegistryFailure::InvalidTexture, 0u,
                          "texture-data-format");
        }
        const auto pixels = static_cast<std::uint64_t>(texture.extent.width) *
                            texture.extent.height;
        auto bytes = pixels * 4u;
        if (bytes != texture.rgba8.size())
            registry_fail(NativePortTextureRegistryFailure::InvalidTexture, 0u,
                          "texture-rgba-size");
        const auto expected_lower_levels = has_mipmaps(
            texture.source_data_format)
                                               ? std::bit_width(
                                                     texture.extent.width) -
                                                     1u
                                               : 0u;
        if (texture.lower_mip_levels.size() != expected_lower_levels)
            registry_fail(NativePortTextureRegistryFailure::InvalidTexture,
                          0u, "texture-mip-count");
        auto expected_extent = texture.extent;
        for (const auto& level : texture.lower_mip_levels) {
            expected_extent.width = std::max(expected_extent.width / 2u, 1u);
            expected_extent.height =
                std::max(expected_extent.height / 2u, 1u);
            const auto level_bytes =
                static_cast<std::uint64_t>(expected_extent.width) *
                expected_extent.height * 4u;
            if (level.extent.width != expected_extent.width ||
                level.extent.height != expected_extent.height ||
                level.rgba8.size() != level_bytes ||
                level_bytes > std::numeric_limits<std::uint64_t>::max() -
                                  bytes)
                registry_fail(
                    NativePortTextureRegistryFailure::InvalidTexture, 0u,
                    "texture-mip-layout");
            bytes += level_bytes;
        }
        return bytes;
    }

    [[nodiscard]] std::size_t find_token_index(
        const std::uint32_t guest_token) const {
        if (guest_token == 0u)
            registry_fail(NativePortTextureRegistryFailure::UnknownToken,
                          guest_token, "token");
        const auto entry = token_index_.find(guest_token);
        if (entry == token_index_.end() || entry->second >= entries_.size() ||
            !entries_[entry->second] ||
            entries_[entry->second]->guest_token != guest_token)
            registry_fail(NativePortTextureRegistryFailure::UnknownToken,
                          guest_token, "token");
        return entry->second;
    }

    [[nodiscard]] Entry& entry_at(const std::size_t index) {
        return *entries_[index];
    }

    [[nodiscard]] const Entry& entry_at(const std::size_t index) const {
        return *entries_[index];
    }

    void erase_entry(const std::size_t index) noexcept {
        auto& entry = *entries_[index];
        identity_index_.erase(entry.identity);
        token_index_.erase(entry.guest_token);
        entries_[index].reset();
        free_slots_.push_back(index);
        --active_entries_;
    }

    NativePortGraphicsDevice& graphics_;
    NativePortTextureRegistryLimits limits_;
    std::vector<std::optional<Entry>> entries_;
    std::vector<std::size_t> free_slots_;
    std::unordered_map<NativePortTextureAssetIdentity, std::size_t,
                       IdentityHash>
        identity_index_;
    std::unordered_map<std::uint32_t, std::size_t> token_index_;
    std::uint32_t next_guest_token_ = 1u;
    std::uint32_t active_entries_ = 0u;
    std::uint64_t texture_bytes_ = 0u;
    std::uint64_t acquired_references_ = 0u;
};

NativePortTextureAssetError::NativePortTextureAssetError(
    const NativePortTextureAssetFailure failure,
    const std::uint64_t byte_offset,
    const std::string_view operation)
    : std::runtime_error(
          "native-port-texture-asset-" + std::string(operation) + ":" +
          std::to_string(static_cast<std::uint32_t>(failure)) + ":" +
          std::to_string(byte_offset)),
      failure_(failure), byte_offset_(byte_offset) {}

NativePortTextureAssetFailure
NativePortTextureAssetError::failure() const noexcept {
    return failure_;
}

std::uint64_t NativePortTextureAssetError::byte_offset() const noexcept {
    return byte_offset_;
}

NativePortTextureBackingStoreError::NativePortTextureBackingStoreError(
    const NativePortTextureBackingStoreFailure failure,
    const std::uint64_t byte_offset,
    const std::string_view operation)
    : std::runtime_error(
          "native-port-texture-backing-" + std::string(operation) + ":" +
          std::to_string(static_cast<std::uint32_t>(failure)) + ":" +
          std::to_string(byte_offset)),
      failure_(failure), byte_offset_(byte_offset) {}

NativePortTextureBackingStoreFailure
NativePortTextureBackingStoreError::failure() const noexcept {
    return failure_;
}

std::uint64_t NativePortTextureBackingStoreError::byte_offset() const noexcept {
    return byte_offset_;
}

NativePortTextureBackingStore::NativePortTextureBackingStore(
    const std::size_t capacity_bytes)
    : impl_(nullptr) {
    try {
        impl_ = std::make_unique<Impl>(capacity_bytes);
    } catch (const NativePortTextureBackingStoreError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw NativePortTextureBackingStoreError(
            NativePortTextureBackingStoreFailure::ResourceExhausted, 0u,
            "allocation");
    }
}

NativePortTextureBackingStore::~NativePortTextureBackingStore() = default;

void NativePortTextureBackingStore::write(
    const std::uint32_t destination,
    const std::span<const std::uint8_t> source) {
    impl_->write(destination, source);
}

NativePortTextureOverlayCoverage NativePortTextureBackingStore::overlay(
    const std::uint32_t source_offset,
    const std::span<std::uint8_t> destination) const {
    return impl_->overlay(source_offset, destination);
}

void NativePortTextureBackingStore::clear() noexcept { impl_->clear(); }

NativePortTextureBackingStoreSnapshot
NativePortTextureBackingStore::snapshot() const noexcept {
    return impl_->snapshot();
}

NativePortTextureRegistryError::NativePortTextureRegistryError(
    const NativePortTextureRegistryFailure failure,
    const std::uint32_t guest_token,
    const std::string_view operation)
    : std::runtime_error(
          "native-port-texture-registry-" + std::string(operation) + ":" +
          std::to_string(static_cast<std::uint32_t>(failure)) + ":" +
          std::to_string(guest_token)),
      failure_(failure), guest_token_(guest_token) {}

NativePortTextureRegistryFailure
NativePortTextureRegistryError::failure() const noexcept {
    return failure_;
}

std::uint32_t NativePortTextureRegistryError::guest_token() const noexcept {
    return guest_token_;
}

NativePortTextureRegistry::NativePortTextureRegistry(
    NativePortGraphicsDevice& graphics,
    const NativePortTextureRegistryLimits& limits)
    : impl_(nullptr) {
    try {
        impl_ = std::make_unique<Impl>(graphics, limits);
    } catch (const NativePortTextureRegistryError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw NativePortTextureRegistryError(
            NativePortTextureRegistryFailure::ResourceExhausted, 0u,
            "allocation");
    }
}

NativePortTextureRegistry::~NativePortTextureRegistry() = default;

NativePortTextureRegistryBinding NativePortTextureRegistry::acquire(
    const NativePortTextureAssetIdentity& identity,
    const NativePortDecodedTextureAsset& texture) {
    try {
        return impl_->acquire(identity, texture);
    } catch (const NativePortTextureRegistryError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw NativePortTextureRegistryError(
            NativePortTextureRegistryFailure::ResourceExhausted, 0u,
            "acquire-allocation");
    } catch (const std::length_error&) {
        throw NativePortTextureRegistryError(
            NativePortTextureRegistryFailure::ResourceExhausted, 0u,
            "acquire-allocation");
    }
}

NativePortTextureHandle NativePortTextureRegistry::resolve(
    const std::uint32_t guest_token,
    const std::uint64_t expected_generation) const {
    return impl_->resolve(guest_token, expected_generation);
}

void NativePortTextureRegistry::release(
    const std::uint32_t guest_token,
    const std::uint64_t expected_generation) {
    impl_->release(guest_token, expected_generation);
}

void NativePortTextureRegistry::invalidate_generation(
    const std::uint64_t generation) {
    impl_->invalidate_generation(generation);
}

void NativePortTextureRegistry::invalidate_all() {
    impl_->invalidate_all();
}

NativePortTextureRegistrySnapshot
NativePortTextureRegistry::snapshot() const noexcept {
    return impl_->snapshot();
}

std::vector<std::uint8_t> decompress_native_port_prs(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] { return decompress_prs_impl(source, limits); }, "prs-allocation");
}

std::vector<NativePortDecodedTextureAsset>
decode_native_port_pvm_texture_archive(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] { return decode_pvm_impl(source, limits); }, "pvm-allocation");
}

std::vector<NativePortDecodedTextureAsset>
decode_native_port_prs_pvm_texture_archive(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] {
            auto decompressed = decompress_prs_impl(source, limits);
            return decode_pvm_impl(decompressed, limits);
        },
        "prs-pvm-allocation");
}

NativePortEncodedTextureAsset extract_native_port_pvm_texture_archive_entry(
    const std::span<const std::uint8_t> source,
    const std::uint32_t archive_ordinal,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] {
            NativePortEncodedTextureAsset captured;
            static_cast<void>(decode_pvm_impl(
                source, limits, archive_ordinal, &captured, false));
            return captured;
        },
        "pvm-entry-extraction");
}

NativePortEncodedTextureAsset
extract_native_port_prs_pvm_texture_archive_entry(
    const std::span<const std::uint8_t> source,
    const std::uint32_t archive_ordinal,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] {
            auto decompressed = decompress_prs_impl(source, limits);
            NativePortEncodedTextureAsset captured;
            static_cast<void>(decode_pvm_impl(
                decompressed, limits, archive_ordinal, &captured, false));
            return captured;
        },
        "prs-pvm-entry-extraction");
}

NativePortEncodedTextureAsset
extract_native_port_prs_pvm_texture_archive_entry(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    const std::uint32_t archive_ordinal,
    const NativePortTextureAssetLimits& limits) {
    if (binding.byte_size == 0u ||
        binding.byte_size > limits.maximum_compressed_bytes)
        fail(NativePortTextureAssetFailure::InvalidLimits, 0u,
             "extract-binding-size");
    return translate_resource_failures(
        [&] {
            auto file = platform.open_content_file(binding);
            std::vector<std::byte> compressed(
                static_cast<std::size_t>(binding.byte_size));
            file->read_at(0u, compressed);
            return extract_native_port_prs_pvm_texture_archive_entry(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(compressed.data()),
                    compressed.size()),
                archive_ordinal, limits);
        },
        "prs-pvm-entry-content");
}

NativePortDecodedTextureAsset decode_native_port_pvr_texture(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] { return decode_pvr_impl(source, limits); }, "pvr-allocation");
}

NativePortDecodedTextureAsset decode_native_port_prs_pvr_texture(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] {
            auto decompressed = decompress_prs_impl(source, limits);
            return decode_pvr_impl(decompressed, limits);
        },
        "prs-pvr-allocation");
}

NativePortDecodedTextureAsset decode_native_port_texture_surface(
    const std::span<const std::uint8_t> source,
    const NativePortExtent extent,
    const NativePortTextureAssetPixelFormat pixel_format,
    const NativePortTextureAssetDataFormat data_format,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures(
        [&] {
            auto texture = decode_surface_impl(source, extent, pixel_format,
                                               data_format, limits);
            bind_decoded_texture_payload_identity(texture);
            return texture;
        },
        "surface-allocation");
}

namespace {

[[nodiscard]] NativePortMaterializedTextureArchive materialize_decoded_textures(
    const std::span<NativePortDecodedTextureAsset> decoded,
    const std::string_view content_byte_identity,
    NativePortTextureRegistry& registry,
    const std::uint64_t generation) {
    NativePortMaterializedTextureArchive archive;
    archive.generation = generation;
    for (std::size_t index = 0u; index < archive.content_sha256.size();
         ++index) {
        const auto hex = content_byte_identity.substr(7u + index * 2u, 2u);
        const auto nibble = [](const char value) -> std::uint8_t {
            if (value >= '0' && value <= '9')
                return static_cast<std::uint8_t>(value - '0');
            if (value >= 'a' && value <= 'f')
                return static_cast<std::uint8_t>(value - 'a' + 10);
            return static_cast<std::uint8_t>(value - 'A' + 10);
        };
        archive.content_sha256[index] = static_cast<std::uint8_t>(
            (nibble(hex[0]) << 4u) | nibble(hex[1]));
    }

    archive.entries.reserve(decoded.size());
    try {
        for (auto& texture : decoded) {
            bind_decoded_texture_payload_identity(texture);
            NativePortTextureAssetIdentity identity;
            identity.generation = generation;
            identity.global_index = texture.global_index;
            identity.archive_ordinal = texture.archive_ordinal;
            identity.content_sha256 = archive.content_sha256;
            NativePortMaterializedTextureAsset materialized{
                texture.name,
                texture.global_index,
                texture.archive_ordinal,
                texture.source_pixel_format,
                texture.source_data_format,
                static_cast<std::uint64_t>(
                    pvr_data_layout(texture.extent,
                                    texture.source_data_format, 0u)
                        .encoded_bytes),
                0u,
                {},
                texture.extent,
                static_cast<std::uint32_t>(
                    texture.lower_mip_levels.size() + 1u),
                texture.decoded_rgba8_sha256,
                texture.decoded_payload_identity_bound};
            const auto acquired = registry.acquire(identity, texture);
            materialized.guest_token = acquired.guest_token;
            materialized.texture = acquired.texture;
            archive.entries.push_back(std::move(materialized));
        }
    } catch (...) {
        const auto acquisition_failure = std::current_exception();
        std::exception_ptr cleanup_failure;
        for (auto entry = archive.entries.rbegin();
             entry != archive.entries.rend(); ++entry) {
            try {
                registry.release(entry->guest_token, generation);
            } catch (...) {
                if (!cleanup_failure) cleanup_failure = std::current_exception();
            }
        }
        if (cleanup_failure) std::rethrow_exception(cleanup_failure);
        std::rethrow_exception(acquisition_failure);
    }
    return archive;
}

void validate_materialization_binding(
    const std::span<const std::uint8_t> source,
    const std::string_view content_byte_identity,
    const std::uint64_t generation,
    const NativePortTextureAssetLimits& limits) {
    if (generation == 0u ||
        !valid_native_port_sha256_identity(content_byte_identity) ||
        source.empty() || source.size() > limits.maximum_compressed_bytes)
        fail(NativePortTextureAssetFailure::InvalidLimits, 0u,
             "materialize-binding");
}

} // namespace

NativePortMaterializedTextureArchive
materialize_native_port_prs_pvm_texture_archive(
    const std::span<const std::uint8_t> source,
    const std::string_view content_byte_identity,
    NativePortTextureRegistry& registry,
    const std::uint64_t generation,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures([&] {
        validate_materialization_binding(source, content_byte_identity,
                                         generation, limits);
        auto decoded = decode_native_port_prs_pvm_texture_archive(
            source, limits);
        return materialize_decoded_textures(
            std::span<NativePortDecodedTextureAsset>(decoded),
            content_byte_identity, registry, generation);
    }, "materialize-prs-pvm");
}

NativePortMaterializedTextureArchive
materialize_native_port_prs_pvm_texture_archive(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    NativePortTextureRegistry& registry,
    const std::uint64_t generation,
    const NativePortTextureAssetLimits& limits) {
    if (binding.byte_size == 0u ||
        binding.byte_size > limits.maximum_compressed_bytes)
        fail(NativePortTextureAssetFailure::InvalidLimits, 0u,
             "materialize-binding-size");
    return translate_resource_failures([&] {
        auto file = platform.open_content_file(binding);
        std::vector<std::byte> compressed(
            static_cast<std::size_t>(binding.byte_size));
        file->read_at(0u, compressed);
        return materialize_native_port_prs_pvm_texture_archive(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(compressed.data()),
                compressed.size()),
            binding.byte_identity, registry, generation, limits);
    }, "materialize-prs-pvm-content");
}

NativePortMaterializedTextureArchive materialize_native_port_pvr_texture(
    const std::span<const std::uint8_t> source,
    const std::string_view content_byte_identity,
    NativePortTextureRegistry& registry,
    const std::uint64_t generation,
    const NativePortTextureAssetLimits& limits) {
    return translate_resource_failures([&] {
        validate_materialization_binding(source, content_byte_identity,
                                         generation, limits);
        auto decoded = decode_native_port_pvr_texture(source, limits);
        return materialize_decoded_textures(
            std::span<NativePortDecodedTextureAsset>(&decoded, 1u),
            content_byte_identity, registry, generation);
    }, "materialize-pvr");
}

NativePortMaterializedTextureArchive materialize_native_port_pvr_texture(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    NativePortTextureRegistry& registry,
    const std::uint64_t generation,
    const NativePortTextureAssetLimits& limits) {
    if (binding.byte_size == 0u ||
        binding.byte_size > limits.maximum_compressed_bytes)
        fail(NativePortTextureAssetFailure::InvalidLimits, 0u,
             "materialize-binding-size");
    return translate_resource_failures([&] {
        auto file = platform.open_content_file(binding);
        std::vector<std::byte> encoded(
            static_cast<std::size_t>(binding.byte_size));
        file->read_at(0u, encoded);
        return materialize_native_port_pvr_texture(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(encoded.data()),
                encoded.size()),
            binding.byte_identity, registry, generation, limits);
    }, "materialize-pvr-content");
}

NativePortTextureGuestDescriptorLayout
native_port_texture_guest_descriptor_layout(
    const NativePortMaterializedTextureAsset& texture) {
    if (texture.extent.width < minimum_pvr_dimension ||
        texture.extent.height < minimum_pvr_dimension ||
        texture.extent.width > maximum_pvr_dimension ||
        texture.extent.height > maximum_pvr_dimension ||
        !std::has_single_bit(texture.extent.width) ||
        !std::has_single_bit(texture.extent.height) ||
        texture.source_encoded_bytes == 0u || texture.mip_levels == 0u)
        fail(NativePortTextureAssetFailure::InvalidDimensions, 0u,
             "guest-descriptor-layout");

    const auto expected_layout = pvr_data_layout(
        texture.extent, texture.source_data_format, 0u);
    const auto expected_mip_levels =
        has_mipmaps(texture.source_data_format)
            ? std::bit_width(texture.extent.width)
            : 1u;
    if (texture.source_encoded_bytes != expected_layout.encoded_bytes ||
        texture.mip_levels != expected_mip_levels)
        fail(NativePortTextureAssetFailure::InvalidPvrt, 0u,
             "guest-descriptor-source-identity");

    const auto width_exponent =
        std::countr_zero(texture.extent.width) - 3u;
    const auto height_exponent =
        std::countr_zero(texture.extent.height) - 3u;
    if (width_exponent > 7u || height_exponent > 7u)
        fail(NativePortTextureAssetFailure::InvalidDimensions, 0u,
             "guest-descriptor-size-bits");

    std::uint32_t tcw =
        static_cast<std::uint32_t>(texture.source_pixel_format) << 27u;
    switch (texture.source_data_format) {
    case NativePortTextureAssetDataFormat::SquareTwiddled:
        break;
    case NativePortTextureAssetDataFormat::SquareTwiddledMipmaps:
        tcw |= 1u << 31u;
        break;
    case NativePortTextureAssetDataFormat::VectorQuantized:
    case NativePortTextureAssetDataFormat::SmallVectorQuantized:
        tcw |= 1u << 30u;
        break;
    case NativePortTextureAssetDataFormat::VectorQuantizedMipmaps:
    case NativePortTextureAssetDataFormat::SmallVectorQuantizedMipmaps:
        tcw |= (1u << 30u) | (1u << 31u);
        break;
    case NativePortTextureAssetDataFormat::Rectangle:
        tcw |= 1u << 26u;
        break;
    default:
        fail(NativePortTextureAssetFailure::UnsupportedDataFormat, 0u,
             "guest-descriptor-data-format");
    }

    return NativePortTextureGuestDescriptorLayout{
        (width_exponent << 3u) | height_exponent,
        tcw,
        texture.extent,
        texture.source_encoded_bytes,
        texture.mip_levels};
}

void release_native_port_texture_archive(
    NativePortTextureRegistry& registry,
    NativePortMaterializedTextureArchive& archive) {
    std::exception_ptr first_failure;
    std::vector<NativePortMaterializedTextureAsset> failed;
    failed.reserve(archive.entries.size());
    for (auto entry = archive.entries.rbegin(); entry != archive.entries.rend();
         ++entry) {
        try {
            registry.release(entry->guest_token, archive.generation);
        } catch (...) {
            if (!first_failure) first_failure = std::current_exception();
            failed.push_back(std::move(*entry));
        }
    }
    std::ranges::reverse(failed);
    archive.entries = std::move(failed);
    if (archive.entries.empty()) {
        archive.generation = 0u;
        archive.content_sha256 = {};
    }
    if (first_failure) std::rethrow_exception(first_failure);
}

} // namespace katana::runtime
