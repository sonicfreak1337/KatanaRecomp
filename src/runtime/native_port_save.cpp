#include "katana/runtime/native_port_save.hpp"

#include "katana/runtime/native_port_platform.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::size_t save_slot_suffix_bytes = std::string_view(".save-c0-s0").size();
static_assert(native_port_save_slot_id_maximum_bytes > save_slot_suffix_bytes);
constexpr std::size_t maximum_provider_id_bytes =
    native_port_save_slot_id_maximum_bytes - save_slot_suffix_bytes;
constexpr std::size_t maximum_profile_identity_bytes = 128u;
constexpr std::size_t maximum_medium_identity_bytes = 96u;
constexpr std::size_t maximum_file_id_bytes = 64u;
constexpr std::size_t maximum_application_id_bytes = 64u;
constexpr std::size_t maximum_title_bytes = 128u;
constexpr std::size_t maximum_description_bytes = 256u;
constexpr std::uint32_t maximum_block_bytes = 64u * 1024u;
constexpr std::uint32_t maximum_block_count = 16'384u;
constexpr std::uint32_t maximum_file_count = 512u;
constexpr std::size_t maximum_unit_count = 4u * 6u;
constexpr std::uint32_t storage_schema_version = 1u;
constexpr std::uint64_t maximum_serialized_volume_header_bytes =
    8u + 4u + 4u + 4u + 2u + maximum_profile_identity_bytes + 2u + maximum_medium_identity_bytes +
    4u;
constexpr std::uint64_t maximum_serialized_file_metadata_bytes =
    2u + maximum_file_id_bytes + 2u + maximum_application_id_bytes + 2u + maximum_title_bytes + 2u +
    maximum_description_bytes + 4u + 4u + 8u;
constexpr std::array<std::byte, 8u> volume_magic{std::byte{'K'},
                                                 std::byte{'N'},
                                                 std::byte{'S'},
                                                 std::byte{'V'},
                                                 std::byte{'O'},
                                                 std::byte{'L'},
                                                 std::byte{0x01u},
                                                 std::byte{0x00u}};
constexpr std::uint32_t volume_format_version = 1u;

[[nodiscard]] bool valid_path_identifier(const std::string_view value,
                                         const std::size_t maximum_bytes) noexcept {
    if (value.empty() || value.size() > maximum_bytes || value.front() == '.' ||
        value.back() == '.' || value.find("..") != std::string_view::npos)
        return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_' ||
               character == '.';
    });
}

[[nodiscard]] bool valid_text(const std::string_view value,
                              const std::size_t maximum_bytes) noexcept {
    if (value.size() > maximum_bytes) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character == '\t' || (character >= 0x20u && character != 0x7Fu);
    });
}

// Identities are stored inside the authenticated volume rather than used as
// paths. Accept the `sha256:<hex>` form already used by native definitions as
// well as title-defined opaque ASCII identities.
[[nodiscard]] bool valid_identity(const std::string_view value,
                                  const std::size_t maximum_bytes) noexcept {
    return !value.empty() && valid_text(value, maximum_bytes);
}

[[nodiscard]] bool valid_endpoint(const NativePortSaveEndpoint endpoint) noexcept {
    return endpoint.controller_index < 4u && endpoint.storage_index < 6u;
}

[[nodiscard]] bool valid_metadata(const NativePortSaveFileMetadata& metadata) noexcept {
    return valid_path_identifier(metadata.file_id, maximum_file_id_bytes) &&
           valid_path_identifier(metadata.application_id, maximum_application_id_bytes) &&
           valid_text(metadata.title, maximum_title_bytes) &&
           valid_text(metadata.description, maximum_description_bytes);
}

[[nodiscard]] std::uint32_t required_blocks(const std::size_t byte_count,
                                            const std::uint32_t block_bytes) noexcept {
    if (byte_count == 0u) return 0u;
    const auto count = static_cast<std::uint64_t>(byte_count);
    return static_cast<std::uint32_t>(1u +
                                      ((count - 1u) / static_cast<std::uint64_t>(block_bytes)));
}

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

class ByteWriter final {
  public:
    void u32(const std::uint32_t value) {
        for (std::size_t index = 0u; index < sizeof(value); ++index)
            bytes_.push_back(std::byte(static_cast<std::uint8_t>(value >> (index * 8u))));
    }

    void u64(const std::uint64_t value) {
        for (std::size_t index = 0u; index < sizeof(value); ++index)
            bytes_.push_back(std::byte(static_cast<std::uint8_t>(value >> (index * 8u))));
    }

    void raw(const std::span<const std::byte> source) {
        bytes_.insert(bytes_.end(), source.begin(), source.end());
    }

    void string(const std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::length_error("native-port-save-string");
        const auto bytes = static_cast<std::uint16_t>(value.size());
        bytes_.push_back(std::byte(static_cast<std::uint8_t>(bytes)));
        bytes_.push_back(std::byte(static_cast<std::uint8_t>(bytes >> 8u)));
        for (const auto character : value)
            bytes_.push_back(std::byte(static_cast<unsigned char>(character)));
    }

    [[nodiscard]] std::vector<std::byte> finish() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::byte> bytes_;
};

class ByteReader final {
  public:
    explicit ByteReader(const std::span<const std::byte> source) : source_(source) {}

    [[nodiscard]] bool u32(std::uint32_t& value) {
        if (remaining() < sizeof(value)) return false;
        value = 0u;
        for (std::size_t index = 0u; index < sizeof(value); ++index)
            value |= static_cast<std::uint32_t>(
                         std::to_integer<std::uint8_t>(source_[position_ + index]))
                     << (index * 8u);
        position_ += sizeof(value);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) {
        if (remaining() < sizeof(value)) return false;
        value = 0u;
        for (std::size_t index = 0u; index < sizeof(value); ++index)
            value |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(source_[position_ + index]))
                     << (index * 8u);
        position_ += sizeof(value);
        return true;
    }

    [[nodiscard]] bool raw(const std::size_t size, std::span<const std::byte>& value) {
        if (size > remaining()) return false;
        value = source_.subspan(position_, size);
        position_ += size;
        return true;
    }

    [[nodiscard]] bool string(std::string& value, const std::size_t maximum_bytes) {
        if (remaining() < 2u) return false;
        const auto size =
            static_cast<std::size_t>(std::to_integer<std::uint8_t>(source_[position_])) |
            (static_cast<std::size_t>(std::to_integer<std::uint8_t>(source_[position_ + 1u]))
             << 8u);
        position_ += 2u;
        if (size > maximum_bytes || size > remaining()) return false;
        value.assign(reinterpret_cast<const char*>(source_.data() + position_), size);
        position_ += size;
        return true;
    }

    [[nodiscard]] bool finished() const noexcept {
        return position_ == source_.size();
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return source_.size() - position_;
    }

  private:
    std::span<const std::byte> source_;
    std::size_t position_ = 0u;
};

struct StoredFile final {
    NativePortSaveDirectoryEntry entry;
    std::vector<std::byte> payload;
};

struct StoredVolume final {
    std::vector<StoredFile> files;
};

[[nodiscard]] std::uint32_t used_blocks(const StoredVolume& volume) noexcept {
    std::uint64_t result = 0u;
    for (const auto& file : volume.files)
        result += file.entry.allocated_blocks;
    return result > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(result);
}

[[nodiscard]] bool volume_is_well_formed(const StoredVolume& volume,
                                         const NativePortSaveUnitConfig& unit) noexcept {
    if (volume.files.size() > unit.maximum_file_count || used_blocks(volume) > unit.block_count)
        return false;
    std::string_view previous;
    for (const auto& file : volume.files) {
        const auto& entry = file.entry;
        if (!valid_path_identifier(entry.file_id, maximum_file_id_bytes) ||
            !valid_path_identifier(entry.application_id, maximum_application_id_bytes) ||
            !valid_text(entry.title, maximum_title_bytes) ||
            !valid_text(entry.description, maximum_description_bytes) ||
            entry.byte_size != file.payload.size() ||
            entry.allocated_blocks != required_blocks(file.payload.size(), unit.block_bytes) ||
            (!previous.empty() && previous >= entry.file_id))
            return false;
        previous = entry.file_id;
    }
    return true;
}

enum class VolumeDecodeResult : std::uint8_t { Valid, Corrupt, Incompatible };

[[nodiscard]] std::vector<std::byte> encode_volume(const StoredVolume& volume,
                                                   const NativePortSaveUnitConfig& unit,
                                                   const std::string_view profile_identity) {
    if (!volume_is_well_formed(volume, unit))
        throw std::invalid_argument("native-port-save-volume");
    ByteWriter writer;
    writer.raw(volume_magic);
    writer.u32(volume_format_version);
    writer.u32(unit.block_bytes);
    writer.u32(unit.block_count);
    writer.string(profile_identity);
    writer.string(unit.identity);
    writer.u32(static_cast<std::uint32_t>(volume.files.size()));
    for (const auto& file : volume.files) {
        writer.string(file.entry.file_id);
        writer.string(file.entry.application_id);
        writer.string(file.entry.title);
        writer.string(file.entry.description);
        writer.u32(file.entry.user_flags);
        writer.u32(file.entry.allocated_blocks);
        writer.u64(file.entry.byte_size);
        writer.raw(file.payload);
    }
    return std::move(writer).finish();
}

[[nodiscard]] VolumeDecodeResult decode_volume(const std::span<const std::byte> bytes,
                                               const NativePortSaveUnitConfig& unit,
                                               const std::string_view profile_identity,
                                               StoredVolume& volume) {
    if (bytes.size() < volume_magic.size() ||
        !std::equal(volume_magic.begin(), volume_magic.end(), bytes.begin()))
        return VolumeDecodeResult::Corrupt;
    ByteReader reader(bytes.subspan(volume_magic.size()));
    std::uint32_t format = 0u;
    std::uint32_t block_bytes = 0u;
    std::uint32_t block_count = 0u;
    std::string stored_profile_identity;
    std::string identity;
    std::uint32_t file_count = 0u;
    if (!reader.u32(format) || !reader.u32(block_bytes) || !reader.u32(block_count) ||
        !reader.string(stored_profile_identity, maximum_profile_identity_bytes) ||
        !reader.string(identity, maximum_medium_identity_bytes) || !reader.u32(file_count) ||
        format != volume_format_version || file_count > unit.maximum_file_count)
        return VolumeDecodeResult::Corrupt;
    if (block_bytes != unit.block_bytes || block_count != unit.block_count ||
        stored_profile_identity != profile_identity || identity != unit.identity)
        return VolumeDecodeResult::Incompatible;
    StoredVolume parsed;
    parsed.files.reserve(file_count);
    for (std::uint32_t index = 0u; index < file_count; ++index) {
        StoredFile file;
        std::uint64_t byte_size = 0u;
        if (!reader.string(file.entry.file_id, maximum_file_id_bytes) ||
            !reader.string(file.entry.application_id, maximum_application_id_bytes) ||
            !reader.string(file.entry.title, maximum_title_bytes) ||
            !reader.string(file.entry.description, maximum_description_bytes) ||
            !reader.u32(file.entry.user_flags) || !reader.u32(file.entry.allocated_blocks) ||
            !reader.u64(byte_size) || byte_size > reader.remaining())
            return VolumeDecodeResult::Corrupt;
        std::span<const std::byte> payload;
        if (!reader.raw(static_cast<std::size_t>(byte_size), payload))
            return VolumeDecodeResult::Corrupt;
        file.entry.byte_size = byte_size;
        file.payload.assign(payload.begin(), payload.end());
        parsed.files.push_back(std::move(file));
    }
    if (!reader.finished() || !volume_is_well_formed(parsed, unit))
        return VolumeDecodeResult::Corrupt;
    volume = std::move(parsed);
    return VolumeDecodeResult::Valid;
}

} // namespace

class NativePortSaveProvider::Impl final {
  public:
    Impl(NativePortPlatformServices& platform, const NativePortSaveProviderConfig& config)
        : platform_(platform), owner_thread_(std::this_thread::get_id()) {
        if (config.contract_version != native_port_save_provider_contract_version ||
            !valid_path_identifier(config.provider_id, maximum_provider_id_bytes) ||
            !valid_identity(config.profile_identity, maximum_profile_identity_bytes) ||
            config.units.size() > maximum_unit_count)
            throw std::invalid_argument("native-port-save-config");
        const auto platform_snapshot = platform_.snapshot();
        if (!platform_snapshot.native_save_backend ||
            platform_snapshot.maximum_save_payload_bytes == 0u)
            throw std::invalid_argument("native-port-save-backend");
        provider_id_.assign(config.provider_id);
        profile_identity_.assign(config.profile_identity);
        units_.reserve(config.units.size());
        unit_identities_.reserve(config.units.size());
        for (const auto& unit : config.units) {
            if (!valid_endpoint(unit.endpoint) || unit.block_bytes == 0u ||
                unit.block_bytes > maximum_block_bytes || unit.block_count == 0u ||
                unit.block_count > maximum_block_count || unit.maximum_file_count == 0u ||
                unit.maximum_file_count > maximum_file_count ||
                (unit.present && !valid_identity(unit.identity, maximum_medium_identity_bytes)))
                throw std::invalid_argument("native-port-save-unit-config");
            if (std::any_of(units_.begin(), units_.end(), [&](const auto& previous) {
                    return previous.endpoint == unit.endpoint;
                }))
                throw std::invalid_argument("native-port-save-duplicate-endpoint");
            const auto capacity = static_cast<std::uint64_t>(unit.block_bytes) *
                                  static_cast<std::uint64_t>(unit.block_count);
            // Account for the full directory metadata against the actual
            // platform-instance budget, so every valid geometry is storable.
            const auto maximum_serialized = capacity + maximum_serialized_volume_header_bytes +
                                            static_cast<std::uint64_t>(unit.maximum_file_count) *
                                                maximum_serialized_file_metadata_bytes;
            if (maximum_serialized > platform_snapshot.maximum_save_payload_bytes)
                throw std::invalid_argument("native-port-save-unit-capacity");
            unit_identities_.emplace_back(unit.identity);
            auto copied = unit;
            copied.identity = unit_identities_.back();
            units_.push_back(copied);
        }
    }

    [[nodiscard]] NativePortSaveQueryResult query(const NativePortSaveQueryRequest& request) {
        require_owner_thread();
        NativePortSaveQueryResult result;
        result.completion = begin(NativePortSaveOperation::Query);
        const auto* unit = find_unit(request.endpoint);
        if (!valid_endpoint(request.endpoint)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
            finish(request.completion, request.completion_user_data, result.completion);
            return result;
        }
        if (unit == nullptr || !unit->present) {
            result.completion.error = NativePortSaveError::Absent;
            finish(request.completion, request.completion_user_data, result.completion);
            return result;
        }
        const auto loaded = load_volume(*unit);
        result.completion.error = loaded.error;
        result.completion.generation = loaded.generation;
        result.completion.platform_error_code = loaded.platform_error_code;
        result.status = make_status(*unit, loaded.volume, loaded.generation);
        finish(request.completion, request.completion_user_data, result.completion);
        return result;
    }

    [[nodiscard]] NativePortSaveListResult list(const NativePortSaveListRequest& request) {
        require_owner_thread();
        NativePortSaveListResult result;
        result.completion = begin(NativePortSaveOperation::List);
        const auto* unit = find_unit(request.endpoint);
        if (!valid_endpoint(request.endpoint)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
        } else if (unit == nullptr || !unit->present) {
            result.completion.error = NativePortSaveError::Absent;
        } else {
            const auto loaded = load_volume(*unit);
            result.completion.error = loaded.error;
            result.completion.generation = loaded.generation;
            result.completion.platform_error_code = loaded.platform_error_code;
            result.status = make_status(*unit, loaded.volume, loaded.generation);
            if (loaded.error == NativePortSaveError::None &&
                !generation_matches(request.expected_generation, loaded.generation)) {
                result.completion.error = NativePortSaveError::GenerationConflict;
            } else if (loaded.error == NativePortSaveError::None) {
                try {
                    result.entries.reserve(loaded.volume.files.size());
                    for (const auto& file : loaded.volume.files)
                        result.entries.push_back(file.entry);
                } catch (const std::bad_alloc&) {
                    result.entries.clear();
                    result.completion.error = NativePortSaveError::ResourceLimit;
                }
            }
        }
        finish(request.completion, request.completion_user_data, result.completion);
        return result;
    }

    [[nodiscard]] NativePortSaveReadResult read(const NativePortSaveReadRequest& request) {
        require_owner_thread();
        NativePortSaveReadResult result;
        result.completion = begin(NativePortSaveOperation::Read);
        const auto* unit = find_unit(request.endpoint);
        if (!valid_endpoint(request.endpoint)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
        } else if (unit == nullptr || !unit->present) {
            result.completion.error = NativePortSaveError::Absent;
        } else if (!valid_path_identifier(request.file_id, maximum_file_id_bytes)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
        } else {
            const auto loaded = load_volume(*unit);
            result.completion.error = loaded.error;
            result.completion.generation = loaded.generation;
            result.completion.platform_error_code = loaded.platform_error_code;
            result.status = make_status(*unit, loaded.volume, loaded.generation);
            if (loaded.error == NativePortSaveError::None &&
                !generation_matches(request.expected_generation, loaded.generation)) {
                result.completion.error = NativePortSaveError::GenerationConflict;
            } else if (loaded.error == NativePortSaveError::None) {
                const auto found = find_file(loaded.volume, request.file_id);
                if (found == loaded.volume.files.end()) {
                    result.completion.error = NativePortSaveError::NotFound;
                } else {
                    if (request.byte_offset > found->payload.size()) {
                        result.completion.error = NativePortSaveError::InvalidArgument;
                    } else {
                        const auto remaining =
                            found->payload.size() - static_cast<std::size_t>(request.byte_offset);
                        const auto requested = request.byte_count == 0u
                                                   ? static_cast<std::uint64_t>(remaining)
                                                   : request.byte_count;
                        if (requested > remaining) {
                            result.completion.error = NativePortSaveError::InvalidArgument;
                        } else {
                            result.completion.required_bytes = requested;
                            if (request.destination.size() < requested) {
                                result.completion.error = NativePortSaveError::BufferTooSmall;
                            } else {
                                const auto start = found->payload.begin() +
                                                   static_cast<std::ptrdiff_t>(request.byte_offset);
                                std::copy_n(start,
                                            static_cast<std::ptrdiff_t>(requested),
                                            request.destination.begin());
                                result.completion.transferred_bytes = requested;
                            }
                        }
                    }
                }
            }
        }
        finish(request.completion, request.completion_user_data, result.completion);
        return result;
    }

    [[nodiscard]] NativePortSaveWriteResult write(const NativePortSaveWriteRequest& request) {
        require_owner_thread();
        NativePortSaveWriteResult result;
        result.completion = begin(NativePortSaveOperation::Write);
        const auto* unit = find_unit(request.endpoint);
        if (!valid_endpoint(request.endpoint)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
        } else if (unit == nullptr || !unit->present) {
            result.completion.error = NativePortSaveError::Absent;
        } else if (!unit->writable) {
            result.completion.error = NativePortSaveError::ReadOnly;
        } else if (!valid_metadata(request.metadata)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
        } else if (request.payload.size() >
                   static_cast<std::uint64_t>(unit->block_bytes) * unit->block_count) {
            result.completion.error = NativePortSaveError::InsufficientBlocks;
        } else {
            auto loaded = load_volume(*unit);
            result.completion.error = loaded.error;
            result.completion.generation = loaded.generation;
            result.completion.platform_error_code = loaded.platform_error_code;
            result.status = make_status(*unit, loaded.volume, loaded.generation);
            if (loaded.error == NativePortSaveError::None &&
                !generation_matches(request.expected_generation, loaded.generation)) {
                result.completion.error = NativePortSaveError::GenerationConflict;
            } else if (loaded.error == NativePortSaveError::None) {
                auto found = find_file(loaded.volume, request.metadata.file_id);
                if (found != loaded.volume.files.end() && !request.replace_existing) {
                    result.completion.error = NativePortSaveError::AlreadyExists;
                } else {
                    try {
                        StoredFile replacement;
                        replacement.entry.file_id = request.metadata.file_id;
                        replacement.entry.application_id = request.metadata.application_id;
                        replacement.entry.title = request.metadata.title;
                        replacement.entry.description = request.metadata.description;
                        replacement.entry.user_flags = request.metadata.user_flags;
                        replacement.entry.byte_size = request.payload.size();
                        replacement.entry.allocated_blocks =
                            required_blocks(request.payload.size(), unit->block_bytes);
                        replacement.payload.assign(request.payload.begin(), request.payload.end());
                        if (found == loaded.volume.files.end()) {
                            if (loaded.volume.files.size() == unit->maximum_file_count) {
                                result.completion.error = NativePortSaveError::DirectoryFull;
                            } else {
                                loaded.volume.files.push_back(std::move(replacement));
                            }
                        } else {
                            *found = std::move(replacement);
                        }
                        if (result.completion.error == NativePortSaveError::None) {
                            sort_files(loaded.volume);
                            if (!volume_is_well_formed(loaded.volume, *unit)) {
                                result.completion.error = NativePortSaveError::InsufficientBlocks;
                            } else {
                                const auto stored = store_volume(*unit, loaded.volume);
                                result.completion.error = stored.error;
                                result.completion.generation = stored.generation;
                                result.completion.platform_error_code = stored.platform_error_code;
                                if (stored.error == NativePortSaveError::None) {
                                    result.completion.transferred_bytes = request.payload.size();
                                    result.status =
                                        make_status(*unit, loaded.volume, stored.generation);
                                }
                            }
                        }
                    } catch (const std::bad_alloc&) {
                        result.completion.error = NativePortSaveError::ResourceLimit;
                    }
                }
            }
        }
        finish(request.completion, request.completion_user_data, result.completion);
        return result;
    }

    [[nodiscard]] NativePortSaveRemoveResult remove(const NativePortSaveRemoveRequest& request) {
        require_owner_thread();
        NativePortSaveRemoveResult result;
        result.completion = begin(NativePortSaveOperation::Remove);
        const auto* unit = find_unit(request.endpoint);
        if (!valid_endpoint(request.endpoint)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
        } else if (unit == nullptr || !unit->present) {
            result.completion.error = NativePortSaveError::Absent;
        } else if (!unit->writable) {
            result.completion.error = NativePortSaveError::ReadOnly;
        } else if (!valid_path_identifier(request.file_id, maximum_file_id_bytes)) {
            result.completion.error = NativePortSaveError::InvalidArgument;
        } else {
            auto loaded = load_volume(*unit);
            result.completion.error = loaded.error;
            result.completion.generation = loaded.generation;
            result.completion.platform_error_code = loaded.platform_error_code;
            result.status = make_status(*unit, loaded.volume, loaded.generation);
            if (loaded.error == NativePortSaveError::None &&
                !generation_matches(request.expected_generation, loaded.generation)) {
                result.completion.error = NativePortSaveError::GenerationConflict;
            } else if (loaded.error == NativePortSaveError::None) {
                const auto found = find_file(loaded.volume, request.file_id);
                if (found == loaded.volume.files.end()) {
                    result.completion.error = NativePortSaveError::NotFound;
                } else {
                    const auto removed_bytes = found->payload.size();
                    loaded.volume.files.erase(found);
                    const auto stored = store_volume(*unit, loaded.volume);
                    result.completion.error = stored.error;
                    result.completion.generation = stored.generation;
                    result.completion.platform_error_code = stored.platform_error_code;
                    if (stored.error == NativePortSaveError::None) {
                        result.completion.transferred_bytes = removed_bytes;
                        result.status = make_status(*unit, loaded.volume, stored.generation);
                    }
                }
            }
        }
        finish(request.completion, request.completion_user_data, result.completion);
        return result;
    }

  private:
    struct LoadedVolume final {
        NativePortSaveError error = NativePortSaveError::None;
        std::uint64_t generation = 0u;
        std::uint32_t platform_error_code = 0u;
        StoredVolume volume;
    };

    [[nodiscard]] NativePortSaveCompletion begin(const NativePortSaveOperation operation) noexcept {
        NativePortSaveCompletion result;
        result.operation = operation;
        result.completion_sequence = next_completion_sequence_;
        saturating_increment(next_completion_sequence_);
        return result;
    }

    static void finish(const NativePortSaveCompletionCallback callback,
                       void* const user_data,
                       const NativePortSaveCompletion& completion) noexcept {
        if (callback != nullptr) callback(user_data, completion);
    }

    [[nodiscard]] const NativePortSaveUnitConfig*
    find_unit(const NativePortSaveEndpoint endpoint) const noexcept {
        if (!valid_endpoint(endpoint)) return nullptr;
        const auto found = std::find_if(units_.begin(), units_.end(), [&](const auto& unit) {
            return unit.endpoint == endpoint;
        });
        return found == units_.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::string storage_slot_id(const NativePortSaveUnitConfig& unit) const {
        return provider_id_ + ".save-c" + std::to_string(unit.endpoint.controller_index) + "-s" +
               std::to_string(unit.endpoint.storage_index);
    }

    [[nodiscard]] LoadedVolume load_volume(const NativePortSaveUnitConfig& unit) const {
        LoadedVolume result;
        try {
            const auto loaded =
                platform_.load_save({storage_slot_id(unit), storage_schema_version});
            result.generation = loaded.generation;
            switch (loaded.status) {
            case NativePortSaveLoadStatus::Missing:
                return result;
            case NativePortSaveLoadStatus::Loaded:
            case NativePortSaveLoadStatus::RecoveredFromBackup:
                switch (decode_volume(loaded.payload, unit, profile_identity_, result.volume)) {
                case VolumeDecodeResult::Valid:
                    break;
                case VolumeDecodeResult::Corrupt:
                    result.error = NativePortSaveError::Corrupt;
                    break;
                case VolumeDecodeResult::Incompatible:
                    result.error = NativePortSaveError::Incompatible;
                    break;
                }
                return result;
            case NativePortSaveLoadStatus::IncompatibleSchema:
                result.error = NativePortSaveError::Incompatible;
                return result;
            case NativePortSaveLoadStatus::Corrupt:
                result.error = NativePortSaveError::Corrupt;
                return result;
            }
        } catch (const NativePortPlatformError& error) {
            result.error = NativePortSaveError::PlatformFailure;
            result.platform_error_code = error.platform_error_code();
        } catch (const std::bad_alloc&) {
            result.error = NativePortSaveError::ResourceLimit;
        } catch (...) {
            result.error = NativePortSaveError::PlatformFailure;
        }
        return result;
    }

    [[nodiscard]] LoadedVolume store_volume(const NativePortSaveUnitConfig& unit,
                                            const StoredVolume& volume) const {
        LoadedVolume result;
        try {
            const auto bytes = encode_volume(volume, unit, profile_identity_);
            result.generation =
                platform_.store_save({storage_slot_id(unit), storage_schema_version}, bytes);
            if (result.generation == 0u) result.error = NativePortSaveError::PlatformFailure;
        } catch (const NativePortPlatformError& error) {
            result.error = NativePortSaveError::PlatformFailure;
            result.platform_error_code = error.platform_error_code();
        } catch (const std::bad_alloc&) {
            result.error = NativePortSaveError::ResourceLimit;
        } catch (const std::length_error&) {
            result.error = NativePortSaveError::ResourceLimit;
        } catch (...) {
            result.error = NativePortSaveError::PlatformFailure;
        }
        return result;
    }

    [[nodiscard]] static NativePortSaveUnitStatus
    make_status(const NativePortSaveUnitConfig& unit,
                const StoredVolume& volume,
                const std::uint64_t generation) noexcept {
        const auto used = used_blocks(volume);
        return {unit.present,
                unit.writable,
                unit.block_bytes,
                unit.block_count,
                used,
                used <= unit.block_count ? unit.block_count - used : 0u,
                static_cast<std::uint32_t>(volume.files.size()),
                generation};
    }

    [[nodiscard]] static bool generation_matches(const std::uint64_t expected,
                                                 const std::uint64_t actual) noexcept {
        return expected == native_port_save_any_generation || expected == actual;
    }

    [[nodiscard]] static std::vector<StoredFile>::iterator
    find_file(StoredVolume& volume, const std::string_view file_id) noexcept {
        return std::find_if(volume.files.begin(), volume.files.end(), [&](const auto& file) {
            return file.entry.file_id == file_id;
        });
    }

    [[nodiscard]] static std::vector<StoredFile>::const_iterator
    find_file(const StoredVolume& volume, const std::string_view file_id) noexcept {
        return std::find_if(volume.files.begin(), volume.files.end(), [&](const auto& file) {
            return file.entry.file_id == file_id;
        });
    }

    static void sort_files(StoredVolume& volume) {
        std::sort(
            volume.files.begin(), volume.files.end(), [](const auto& left, const auto& right) {
                return left.entry.file_id < right.entry.file_id;
            });
    }

    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_)
            throw std::logic_error("native-port-save-thread");
    }

    NativePortPlatformServices& platform_;
    std::thread::id owner_thread_;
    std::string provider_id_;
    std::string profile_identity_;
    std::vector<NativePortSaveUnitConfig> units_;
    std::vector<std::string> unit_identities_;
    std::uint64_t next_completion_sequence_ = 1u;
};

NativePortSaveProvider::NativePortSaveProvider(NativePortPlatformServices& platform,
                                               const NativePortSaveProviderConfig& config)
    : impl_(std::make_unique<Impl>(platform, config)) {}

NativePortSaveProvider::~NativePortSaveProvider() = default;

NativePortSaveQueryResult NativePortSaveProvider::query(const NativePortSaveQueryRequest& request) {
    return impl_->query(request);
}

NativePortSaveListResult NativePortSaveProvider::list(const NativePortSaveListRequest& request) {
    return impl_->list(request);
}

NativePortSaveReadResult NativePortSaveProvider::read(const NativePortSaveReadRequest& request) {
    return impl_->read(request);
}

NativePortSaveWriteResult NativePortSaveProvider::write(const NativePortSaveWriteRequest& request) {
    return impl_->write(request);
}

NativePortSaveRemoveResult
NativePortSaveProvider::remove(const NativePortSaveRemoveRequest& request) {
    return impl_->remove(request);
}

} // namespace katana::runtime
