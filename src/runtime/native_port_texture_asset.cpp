#include "katana/runtime/native_port_texture_asset.hpp"

#include "prs_decode.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint32_t pvm_magic = 0x484D5650u;
constexpr std::uint32_t pvrt_magic = 0x54525650u;
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
constexpr std::size_t maximum_pvrt_padding_bytes = 31u;

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
    case 0x09u:
        return NativePortTextureAssetDataFormat::Rectangle;
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
    if (!basic_valid ||
        (data_format == NativePortTextureAssetDataFormat::SquareTwiddled &&
         extent.width != extent.height))
        fail(NativePortTextureAssetFailure::InvalidDimensions, offset,
             "pvrt-dimensions");
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

void decode_pixels(const std::span<const std::uint8_t> source,
                   NativePortDecodedTextureAsset& texture) {
    const auto width = texture.extent.width;
    const auto height = texture.extent.height;
    for (std::uint32_t y = 0u; y < height; ++y) {
        for (std::uint32_t x = 0u; x < width; ++x) {
            const std::size_t source_pixel =
                texture.source_data_format ==
                        NativePortTextureAssetDataFormat::SquareTwiddled
                    ? morton_index(x, y, width)
                    : static_cast<std::size_t>(y) * width + x;
            const std::size_t source_offset = source_pixel * 2u;
            const std::uint16_t value =
                static_cast<std::uint16_t>(source[source_offset]) |
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(source[source_offset + 1u])
                    << 8u);
            const std::size_t destination_offset =
                (static_cast<std::size_t>(y) * width + x) * 4u;
            decode_pixel(value, texture.source_pixel_format,
                         texture.rgba8.data() + destination_offset);
        }
    }
}

[[nodiscard]] std::vector<NativePortDecodedTextureAsset> decode_pvm_impl(
    const std::span<const std::uint8_t> source,
    const NativePortTextureAssetLimits& limits) {
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
        const auto encoded_bytes = checked_multiply(
            pixel_count, 2u, NativePortTextureAssetFailure::InvalidPvrt,
            chunk_offset + 4u, "pvrt-pixel-bytes");
        const auto rgba_bytes = checked_multiply(
            pixel_count, 4u,
            NativePortTextureAssetFailure::RgbaOutputLimit,
            chunk_offset + 12u, "pvrt-rgba-bytes");
        const auto available_data_bytes = chunk_payload_size - 8u;
        if (encoded_bytes > available_data_bytes)
            fail(NativePortTextureAssetFailure::InvalidPvrt,
                 chunk_offset + 4u, "pvrt-truncated-pixels");
        const auto padding_bytes = available_data_bytes - encoded_bytes;
        const auto pixel_data_offset = chunk_offset + 16u;
        const auto unpadded_chunk_bytes = checked_add(
            16u, encoded_bytes,
            NativePortTextureAssetFailure::InvalidPvrt, chunk_offset + 4u,
            "pvrt-padding-size");
        const auto expected_padding_bytes =
            (32u - (unpadded_chunk_bytes & 31u)) & 31u;
        if (padding_bytes > maximum_pvrt_padding_bytes ||
            padding_bytes != expected_padding_bytes ||
            !all_zero(source.subspan(pixel_data_offset + encoded_bytes,
                                     padding_bytes)))
            fail(NativePortTextureAssetFailure::InvalidPvrt,
                 pixel_data_offset + encoded_bytes, "pvrt-padding");
        aggregate_rgba_bytes = checked_add(
            aggregate_rgba_bytes, rgba_bytes,
            NativePortTextureAssetFailure::RgbaOutputLimit, chunk_offset,
            "pvm-rgba-size");
        if (aggregate_rgba_bytes > limits.maximum_rgba_bytes)
            fail(NativePortTextureAssetFailure::RgbaOutputLimit,
                 chunk_offset, "pvm-rgba-limit");

        texture.rgba8.resize(rgba_bytes);
        decode_pixels(source.subspan(pixel_data_offset, encoded_bytes), texture);
        textures.push_back(std::move(texture));
        chunk_offset = chunk_end;
    }

    if (chunk_offset != source.size())
        fail(NativePortTextureAssetFailure::InvalidPvm, chunk_offset,
             "pvm-trailing-data");
    return textures;
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
        if (identity.global_index != texture.global_index)
            registry_fail(NativePortTextureRegistryFailure::InvalidIdentity,
                          0u, "identity-global-index");
        const auto texture_bytes = validate_texture(texture);

        const auto existing = std::ranges::find_if(
            entries_, [&identity](const Entry& entry) {
                return entry.identity == identity;
            });
        if (existing != entries_.end()) {
            if (existing->extent.width != texture.extent.width ||
                existing->extent.height != texture.extent.height ||
                existing->pixel_format != texture.source_pixel_format ||
                existing->data_format != texture.source_data_format ||
                existing->texture_bytes != texture_bytes)
                registry_fail(
                    NativePortTextureRegistryFailure::IdentityCollision,
                    existing->guest_token, "identity-collision");
            if (existing->references ==
                std::numeric_limits<std::uint32_t>::max())
                registry_fail(
                    NativePortTextureRegistryFailure::ReferenceCountLimit,
                    existing->guest_token, "reference-count");
            if (acquired_references_ ==
                std::numeric_limits<std::uint64_t>::max())
                registry_fail(
                    NativePortTextureRegistryFailure::ReferenceCountLimit,
                    existing->guest_token, "aggregate-reference-count");
            ++existing->references;
            ++acquired_references_;
            return {existing->guest_token, existing->handle};
        }

        // A GBIX identifies one logical texture inside a content generation.
        // A different verified content digest for that same identity is
        // ambiguous and cannot silently create a second native mapping.
        if (identity.global_index.has_value()) {
            const auto collision = std::ranges::find_if(
                entries_, [&identity](const Entry& entry) {
                    return entry.identity.generation == identity.generation &&
                           entry.identity.global_index == identity.global_index;
                });
            if (collision != entries_.end())
                registry_fail(
                    NativePortTextureRegistryFailure::IdentityCollision,
                    collision->guest_token, "content-digest-collision");
        }

        if (entries_.size() == limits_.maximum_entries)
            registry_fail(NativePortTextureRegistryFailure::EntryLimit, 0u,
                          "entry-limit");
        if (texture_bytes > limits_.maximum_texture_bytes - texture_bytes_)
            registry_fail(NativePortTextureRegistryFailure::ByteLimit, 0u,
                          "byte-limit");
        if (next_guest_token_ == 0u)
            registry_fail(NativePortTextureRegistryFailure::TokenExhausted,
                          0u, "token-exhausted");

        NativePortTextureConfig config;
        config.extent = texture.extent;
        config.format = NativePortTextureFormat::Rgba8Unorm;
        NativePortImageView image;
        image.extent = texture.extent;
        image.format = NativePortTextureFormat::Rgba8Unorm;
        image.stride_bytes = texture.extent.width * 4u;
        image.pixels = std::as_bytes(std::span(texture.rgba8));
        const auto handle = graphics_.create_texture(config, &image);
        const auto guest_token = next_guest_token_++;
        try {
            entries_.push_back(Entry{identity,
                                     guest_token,
                                     handle,
                                     1u,
                                     texture_bytes,
                                     texture.extent,
                                     texture.source_pixel_format,
                                     texture.source_data_format});
        } catch (...) {
            graphics_.destroy_texture(handle);
            throw;
        }
        texture_bytes_ += texture_bytes;
        ++acquired_references_;
        return {guest_token, handle};
    }

    [[nodiscard]] NativePortTextureHandle resolve(
        const std::uint32_t guest_token,
        const std::uint64_t expected_generation) const {
        const auto entry = find_token(guest_token);
        if (entry->identity.generation != expected_generation)
            registry_fail(NativePortTextureRegistryFailure::GenerationMismatch,
                          guest_token, "resolve-generation");
        return entry->handle;
    }

    void release(const std::uint32_t guest_token,
                 const std::uint64_t expected_generation) {
        const auto entry = find_token(guest_token);
        if (entry->identity.generation != expected_generation)
            registry_fail(NativePortTextureRegistryFailure::GenerationMismatch,
                          guest_token, "release-generation");
        if (entry->references > 1u) {
            --entry->references;
            --acquired_references_;
            return;
        }
        graphics_.destroy_texture(entry->handle);
        texture_bytes_ -= entry->texture_bytes;
        --acquired_references_;
        entries_.erase(entry);
    }

    void invalidate_generation(const std::uint64_t generation) {
        if (generation == 0u)
            registry_fail(NativePortTextureRegistryFailure::InvalidIdentity,
                          0u, "invalidate-generation");
        for (auto entry = entries_.begin(); entry != entries_.end();) {
            if (entry->identity.generation != generation) {
                ++entry;
                continue;
            }
            graphics_.destroy_texture(entry->handle);
            texture_bytes_ -= entry->texture_bytes;
            acquired_references_ -= entry->references;
            entry = entries_.erase(entry);
        }
    }

    void invalidate_all() {
        for (auto entry = entries_.begin(); entry != entries_.end();) {
            graphics_.destroy_texture(entry->handle);
            texture_bytes_ -= entry->texture_bytes;
            acquired_references_ -= entry->references;
            entry = entries_.erase(entry);
        }
    }

    [[nodiscard]] NativePortTextureRegistrySnapshot snapshot() const noexcept {
        return {static_cast<std::uint32_t>(entries_.size()), texture_bytes_,
                acquired_references_};
    }

  private:
    struct Entry final {
        NativePortTextureAssetIdentity identity;
        std::uint32_t guest_token;
        NativePortTextureHandle handle;
        std::uint32_t references;
        std::uint64_t texture_bytes;
        NativePortExtent extent;
        NativePortTextureAssetPixelFormat pixel_format;
        NativePortTextureAssetDataFormat data_format;
    };

    using EntryIterator = std::vector<Entry>::iterator;
    using ConstEntryIterator = std::vector<Entry>::const_iterator;

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
        case NativePortTextureAssetDataFormat::Rectangle:
            break;
        default:
            registry_fail(NativePortTextureRegistryFailure::InvalidTexture, 0u,
                          "texture-data-format");
        }
        const auto pixels = static_cast<std::uint64_t>(texture.extent.width) *
                            texture.extent.height;
        const auto bytes = pixels * 4u;
        if (bytes != texture.rgba8.size())
            registry_fail(NativePortTextureRegistryFailure::InvalidTexture, 0u,
                          "texture-rgba-size");
        return bytes;
    }

    [[nodiscard]] EntryIterator find_token(const std::uint32_t guest_token) {
        if (guest_token == 0u)
            registry_fail(NativePortTextureRegistryFailure::UnknownToken,
                          guest_token, "token");
        const auto entry = std::ranges::find_if(
            entries_, [guest_token](const Entry& candidate) {
                return candidate.guest_token == guest_token;
            });
        if (entry == entries_.end())
            registry_fail(NativePortTextureRegistryFailure::UnknownToken,
                          guest_token, "token");
        return entry;
    }

    [[nodiscard]] ConstEntryIterator find_token(
        const std::uint32_t guest_token) const {
        if (guest_token == 0u)
            registry_fail(NativePortTextureRegistryFailure::UnknownToken,
                          guest_token, "token");
        const auto entry = std::ranges::find_if(
            entries_, [guest_token](const Entry& candidate) {
                return candidate.guest_token == guest_token;
            });
        if (entry == entries_.end())
            registry_fail(NativePortTextureRegistryFailure::UnknownToken,
                          guest_token, "token");
        return entry;
    }

    NativePortGraphicsDevice& graphics_;
    NativePortTextureRegistryLimits limits_;
    std::vector<Entry> entries_;
    std::uint32_t next_guest_token_ = 1u;
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
    return impl_->acquire(identity, texture);
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

} // namespace katana::runtime
