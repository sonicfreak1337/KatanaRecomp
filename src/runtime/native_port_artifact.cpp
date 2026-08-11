#include "katana/runtime/native_port_artifact.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::array<std::uint8_t, 8u> artifact_magic{
    'K', 'A', 'T', 'N', 'P', 'R', '1', '\n'};
constexpr std::uint32_t artifact_header_size = 88u;
constexpr std::uint32_t artifact_maximum_entries = 65'536u;
constexpr std::uint32_t artifact_maximum_string_size = 4096u;
constexpr std::uint32_t artifact_maximum_identity_size = 128u;
constexpr std::size_t artifact_sha256_size = 64u;
constexpr std::string_view sha256_prefix = "sha256:";

[[noreturn]] void artifact_error(const std::string_view message) {
    throw std::runtime_error(std::string(message));
}

[[nodiscard]] bool lower_hex(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] std::string hash_bytes(
    const std::span<const std::uint8_t> bytes) {
    const auto text =
        bytes.empty()
            ? std::string_view{}
            : std::string_view(
                  reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return io::sha256_bytes(text);
}

class BinaryWriter final {
  public:
    explicit BinaryWriter(const std::size_t maximum_size)
        : maximum_size_(maximum_size) {}

    void u8(const std::uint8_t value) {
        append(1u);
        bytes_.push_back(value);
    }

    void u32(const std::uint32_t value) {
        append(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u64(const std::uint64_t value) {
        append(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void boolean(const bool value) {
        u8(value ? 1u : 0u);
    }

    template <typename Enum>
    void enumeration(const Enum value) {
        static_assert(std::is_enum_v<Enum>);
        u32(static_cast<std::uint32_t>(value));
    }

    void string(const std::string_view value,
                const std::uint32_t maximum_size =
                    artifact_maximum_string_size) {
        if (value.size() > maximum_size)
            artifact_error("Native-port artifact string is too long.");
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value);
    }

    void raw(const std::string_view value) {
        append(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void raw(const std::span<const std::uint8_t> value) {
        append(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    void append(const std::size_t size) {
        if (size > maximum_size_ - bytes_.size())
            artifact_error("Native-port artifact exceeds its hard size limit.");
    }

    std::size_t maximum_size_;
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader final {
  public:
    explicit BinaryReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        return take(1u)[0];
    }

    [[nodiscard]] std::uint32_t u32() {
        const auto bytes = take(sizeof(std::uint32_t));
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < bytes.size(); ++byte)
            value |= static_cast<std::uint32_t>(bytes[byte])
                     << (byte * 8u);
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        const auto bytes = take(sizeof(std::uint64_t));
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < bytes.size(); ++byte)
            value |= static_cast<std::uint64_t>(bytes[byte])
                     << (byte * 8u);
        return value;
    }

    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            artifact_error("Native-port artifact boolean is invalid.");
        return value != 0u;
    }

    template <typename Enum>
    [[nodiscard]] Enum enumeration() {
        static_assert(std::is_enum_v<Enum>);
        using Underlying = std::underlying_type_t<Enum>;
        using Unsigned = std::make_unsigned_t<Underlying>;
        const auto value = u32();
        if (value > static_cast<std::uint32_t>(
                        std::numeric_limits<Unsigned>::max()))
            artifact_error("Native-port artifact enum encoding is invalid.");
        return static_cast<Enum>(
            static_cast<Underlying>(static_cast<Unsigned>(value)));
    }

    [[nodiscard]] std::string string(
        const std::uint32_t maximum_size =
            artifact_maximum_string_size) {
        const auto size = u32();
        if (size > maximum_size)
            artifact_error("Native-port artifact string is too long.");
        const auto bytes = take(size);
        return std::string(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    [[nodiscard]] std::uint32_t count(
        const std::uint32_t maximum = artifact_maximum_entries) {
        const auto value = u32();
        if (value > maximum)
            artifact_error("Native-port artifact count exceeds its limit.");
        return value;
    }

    [[nodiscard]] std::span<const std::uint8_t> take(
        const std::size_t size) {
        if (offset_ > bytes_.size() || size > bytes_.size() - offset_)
            artifact_error("Native-port artifact is truncated.");
        const auto result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    [[nodiscard]] bool empty() const noexcept {
        return offset_ == bytes_.size();
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
};

[[nodiscard]] std::uint32_t checked_count(const std::size_t value) {
    if (value > artifact_maximum_entries)
        artifact_error("Native-port artifact count exceeds its limit.");
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::vector<std::uint8_t>
serialize_definition(const NativePortDefinition& definition) {
    BinaryWriter writer(static_cast<std::size_t>(
        native_port_artifact_maximum_size - artifact_header_size));
    writer.u32(definition.contract_version);
    writer.string(definition.project_id);
    writer.string(definition.project_version);
    writer.string(definition.executable.content_identity,
                  artifact_maximum_identity_size);
    writer.string(definition.executable.executable_name, 255u);
    writer.string(definition.executable.executable_byte_identity,
                  artifact_maximum_identity_size);
    writer.u32(definition.bootstrap.entry_point);
    writer.u32(definition.bootstrap.stack_pointer);
    writer.u32(definition.bootstrap.vector_base);
    writer.u32(definition.bootstrap.status_register);
    writer.u32(definition.bootstrap.fpscr);
    writer.string(definition.bootstrap.symbol);

    writer.u32(checked_count(definition.images.size()));
    for (const auto& image : definition.images) {
        writer.string(image.image_id);
        writer.string(image.content_relative_path);
        writer.string(image.byte_identity, artifact_maximum_identity_size);
        writer.u64(image.file_offset);
        writer.u32(image.guest_address);
        writer.u32(image.byte_size);
        writer.boolean(image.writable);
    }

    writer.u32(checked_count(definition.hooks.size()));
    for (const auto& hook : definition.hooks) {
        writer.u32(hook.guest_address);
        writer.u32(hook.covered_size);
        writer.enumeration(hook.kind);
        writer.enumeration(hook.requirement);
        writer.enumeration(hook.original_policy);
        writer.string(hook.symbol);
        writer.string(hook.code_identity, artifact_maximum_identity_size);
    }

    writer.u32(checked_count(definition.hardware_resolutions.size()));
    for (const auto& resolution : definition.hardware_resolutions) {
        writer.u32(resolution.instruction_address);
        writer.enumeration(resolution.kind);
        writer.u32(resolution.hook_guest_address);
        writer.string(resolution.native_memory_image_id);
        writer.u32(resolution.native_memory_guest_address);
        writer.u32(resolution.native_memory_byte_size);
        writer.u8(resolution.native_memory_access_mask);
        writer.u8(resolution.native_memory_width_mask);
    }
    return std::move(writer).finish();
}

[[nodiscard]] std::vector<std::uint8_t>
encode_artifact(const NativePortDefinition& definition) {
    const auto payload = serialize_definition(definition);
    const auto payload_sha256 = hash_bytes(payload);
    BinaryWriter writer(
        static_cast<std::size_t>(native_port_artifact_maximum_size));
    writer.raw(std::span<const std::uint8_t>(artifact_magic));
    writer.u32(native_port_artifact_format_version);
    writer.u32(artifact_header_size);
    writer.u64(payload.size());
    writer.raw(payload_sha256);
    writer.raw(payload);
    return std::move(writer).finish();
}

void require_regular_nonsymlink_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        artifact_error(
            "Native-port artifact must be a regular non-symlink file.");
}

[[nodiscard]] std::filesystem::path canonical_regular_file(
    const std::filesystem::path& path) {
    if (path.empty()) artifact_error("Native-port artifact path is empty.");
    require_regular_nonsymlink_file(path);
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error)
        artifact_error("Native-port artifact path cannot be canonicalized.");
    require_regular_nonsymlink_file(canonical);
    return canonical;
}

[[nodiscard]] std::vector<std::uint8_t> read_artifact_file(
    const std::filesystem::path& path) {
    require_regular_nonsymlink_file(path);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) artifact_error("Native-port artifact cannot be opened.");
    const auto end = input.tellg();
    if (end < 0 ||
        static_cast<std::uint64_t>(end) > native_port_artifact_maximum_size)
        artifact_error("Native-port artifact exceeds its hard size limit.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    if (!input) artifact_error("Native-port artifact cannot be read.");
    require_regular_nonsymlink_file(path);
    std::error_code error;
    if (std::filesystem::file_size(path, error) != bytes.size() || error)
        artifact_error("Native-port artifact changed while it was read.");
    return bytes;
}

[[nodiscard]] std::filesystem::path normalized_destination(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty() || path.filename() == "." ||
        path.filename() == "..")
        artifact_error("Native-port artifact destination is invalid.");
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(absolute.parent_path());
    std::error_code error;
    const auto parent =
        std::filesystem::canonical(absolute.parent_path(), error);
    if (error)
        artifact_error(
            "Native-port artifact destination cannot be canonicalized.");
    const auto parent_status = std::filesystem::symlink_status(parent, error);
    if (error || !std::filesystem::is_directory(parent_status) ||
        std::filesystem::is_symlink(parent_status))
        artifact_error(
            "Native-port artifact parent is not a canonical directory.");
    const auto result = parent / absolute.filename();
    const auto status = std::filesystem::symlink_status(result, error);
    const auto missing =
        (!error && status.type() == std::filesystem::file_type::not_found) ||
        error == std::errc::no_such_file_or_directory;
    if (!missing && !error &&
        (std::filesystem::is_symlink(status) ||
         !std::filesystem::is_regular_file(status)))
        artifact_error(
            "Native-port artifact destination is not a regular file.");
    if (!missing && error)
        artifact_error(
            "Native-port artifact destination cannot be inspected.");
    return result;
}

[[nodiscard]] std::filesystem::path temporary_path(
    const std::filesystem::path& destination) {
    for (std::size_t attempt = 1u; attempt <= 1024u; ++attempt) {
        auto candidate = destination;
        candidate += ".tmp-" + std::to_string(attempt);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if ((!error &&
             status.type() == std::filesystem::file_type::not_found) ||
            error == std::errc::no_such_file_or_directory)
            return candidate;
        if (error)
            artifact_error(
                "Temporary native-port artifact path cannot be inspected.");
        if (std::filesystem::is_symlink(status))
            artifact_error("Temporary native-port artifact path is a symlink.");
    }
    artifact_error("No temporary native-port artifact path is available.");
}

void durable_write(const std::filesystem::path& path,
                   const std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    std::FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0) file = nullptr;
#else
    auto* file = std::fopen(path.c_str(), "wb");
#endif
    if (file == nullptr)
        artifact_error("Temporary native-port artifact cannot be created.");
    try {
        const auto written =
            bytes.empty()
                ? 0u
                : std::fwrite(bytes.data(), 1u, bytes.size(), file);
        if (written != bytes.size() || std::fflush(file) != 0)
            artifact_error("Temporary native-port artifact cannot be written.");
#ifdef _WIN32
        if (::_commit(::_fileno(file)) != 0)
#else
        if (::fsync(::fileno(file)) != 0)
#endif
            artifact_error(
                "Temporary native-port artifact cannot be synchronized.");
    } catch (...) {
        static_cast<void>(std::fclose(file));
        throw;
    }
    if (std::fclose(file) != 0)
        artifact_error("Temporary native-port artifact cannot be closed.");
}

void atomic_replace(const std::filesystem::path& source,
                    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!::MoveFileExW(source.c_str(),
                       destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        artifact_error("Native-port artifact cannot be atomically published.");
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error)
        artifact_error("Native-port artifact cannot be atomically published.");
#endif
}

} // namespace

void NativePortArtifact::rebuild_definition() {
    if (image_ids_.size() != images_.size() ||
        image_paths_.size() != images_.size() ||
        image_byte_identities_.size() != images_.size() ||
        hook_symbols_.size() != hooks_.size() ||
        hook_code_identities_.size() != hooks_.size() ||
        hardware_resolution_image_ids_.size() !=
            hardware_resolutions_.size())
        artifact_error("Native-port artifact ownership is inconsistent.");

    bootstrap_.symbol = bootstrap_symbol_;
    for (std::size_t index = 0u; index < images_.size(); ++index) {
        images_[index].image_id = image_ids_[index];
        images_[index].content_relative_path = image_paths_[index];
        images_[index].byte_identity = image_byte_identities_[index];
    }
    for (std::size_t index = 0u; index < hooks_.size(); ++index) {
        hooks_[index].symbol = hook_symbols_[index];
        hooks_[index].code_identity = hook_code_identities_[index];
    }
    for (std::size_t index = 0u; index < hardware_resolutions_.size(); ++index)
        hardware_resolutions_[index].native_memory_image_id =
            hardware_resolution_image_ids_[index];

    definition_ = {};
    definition_.contract_version = contract_version_;
    definition_.project_id = project_id_;
    definition_.project_version = project_version_;
    definition_.executable = {executable_content_identity_, executable_name_,
                              executable_byte_identity_};
    definition_.bootstrap = bootstrap_;
    definition_.images = images_;
    definition_.hooks = hooks_;
    definition_.hardware_resolutions = hardware_resolutions_;
    validate_native_port_definition(definition_);
}

std::shared_ptr<NativePortArtifact>
NativePortArtifact::load(const std::filesystem::path& path) {
    const auto canonical = canonical_regular_file(path);
    const auto bytes = read_artifact_file(canonical);
    if (bytes.size() < artifact_header_size)
        artifact_error("Native-port artifact header is invalid.");

    BinaryReader file_reader(bytes);
    const auto magic = file_reader.take(artifact_magic.size());
    if (!std::equal(magic.begin(), magic.end(), artifact_magic.begin()))
        artifact_error("Native-port artifact magic is invalid.");
    const auto format_version = file_reader.u32();
    const auto header_size = file_reader.u32();
    const auto payload_size = file_reader.u64();
    const auto payload_sha_bytes = file_reader.take(artifact_sha256_size);
    const std::string payload_sha256(
        reinterpret_cast<const char*>(payload_sha_bytes.data()),
        payload_sha_bytes.size());
    const auto actual_payload_size =
        static_cast<std::uint64_t>(bytes.size() - artifact_header_size);
    if (format_version != native_port_artifact_format_version ||
        header_size != artifact_header_size || payload_size != actual_payload_size ||
        !lower_hex(payload_sha256))
        artifact_error("Native-port artifact header is invalid.");
    const auto payload = file_reader.take(static_cast<std::size_t>(payload_size));
    if (!file_reader.empty() || hash_bytes(payload) != payload_sha256)
        artifact_error("Native-port artifact SHA-256 is invalid.");

    auto result = std::shared_ptr<NativePortArtifact>(new NativePortArtifact);
    result->canonical_path_ = canonical;
    result->artifact_identity_ = std::string(sha256_prefix) + hash_bytes(bytes);
    BinaryReader reader(payload);
    result->contract_version_ = reader.u32();
    result->project_id_ = reader.string();
    result->project_version_ = reader.string();
    result->executable_content_identity_ =
        reader.string(artifact_maximum_identity_size);
    result->executable_name_ = reader.string(255u);
    result->executable_byte_identity_ =
        reader.string(artifact_maximum_identity_size);
    result->bootstrap_.entry_point = reader.u32();
    result->bootstrap_.stack_pointer = reader.u32();
    result->bootstrap_.vector_base = reader.u32();
    result->bootstrap_.status_register = reader.u32();
    result->bootstrap_.fpscr = reader.u32();
    result->bootstrap_symbol_ = reader.string();

    const auto image_count = reader.count();
    result->image_ids_.reserve(image_count);
    result->image_paths_.reserve(image_count);
    result->image_byte_identities_.reserve(image_count);
    result->images_.reserve(image_count);
    for (std::uint32_t index = 0u; index < image_count; ++index) {
        NativePortImageBinding image;
        result->image_ids_.push_back(reader.string());
        result->image_paths_.push_back(reader.string());
        result->image_byte_identities_.push_back(
            reader.string(artifact_maximum_identity_size));
        image.file_offset = reader.u64();
        image.guest_address = reader.u32();
        image.byte_size = reader.u32();
        image.writable = reader.boolean();
        result->images_.push_back(image);
    }

    const auto hook_count = reader.count();
    result->hook_symbols_.reserve(hook_count);
    result->hook_code_identities_.reserve(hook_count);
    result->hooks_.reserve(hook_count);
    for (std::uint32_t index = 0u; index < hook_count; ++index) {
        NativePortHookBinding hook;
        hook.guest_address = reader.u32();
        hook.covered_size = reader.u32();
        hook.kind = reader.enumeration<NativePortHookKind>();
        hook.requirement = reader.enumeration<NativePortHookRequirement>();
        hook.original_policy =
            reader.enumeration<NativePortHookOriginalPolicy>();
        result->hook_symbols_.push_back(reader.string());
        result->hook_code_identities_.push_back(
            reader.string(artifact_maximum_identity_size));
        result->hooks_.push_back(hook);
    }

    const auto resolution_count = reader.count();
    result->hardware_resolution_image_ids_.reserve(resolution_count);
    result->hardware_resolutions_.reserve(resolution_count);
    for (std::uint32_t index = 0u; index < resolution_count; ++index) {
        NativePortHardwareResolution resolution;
        resolution.instruction_address = reader.u32();
        resolution.kind =
            reader.enumeration<NativePortHardwareResolutionKind>();
        resolution.hook_guest_address = reader.u32();
        result->hardware_resolution_image_ids_.push_back(reader.string());
        resolution.native_memory_guest_address = reader.u32();
        resolution.native_memory_byte_size = reader.u32();
        resolution.native_memory_access_mask = reader.u8();
        resolution.native_memory_width_mask = reader.u8();
        result->hardware_resolutions_.push_back(resolution);
    }

    if (!reader.empty())
        artifact_error("Native-port artifact has unexpected trailing data.");
    result->rebuild_definition();
    return result;
}

std::shared_ptr<NativePortArtifact>
NativePortArtifact::write(const std::filesystem::path& path,
                          const NativePortDefinition& definition) {
    validate_native_port_definition(definition);
    const auto bytes = encode_artifact(definition);
    const auto expected_artifact_identity =
        std::string(sha256_prefix) + hash_bytes(bytes);
    const auto destination = normalized_destination(path);
    const auto temporary = temporary_path(destination);
    try {
        durable_write(temporary, bytes);
        const auto reread = load(temporary);
        if (reread->artifact_identity() != expected_artifact_identity)
            artifact_error(
                "Temporary native-port artifact validation changed its content.");
        atomic_replace(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return load(destination);
}

const std::filesystem::path&
NativePortArtifact::canonical_path() const noexcept {
    return canonical_path_;
}

const std::string& NativePortArtifact::artifact_identity() const noexcept {
    return artifact_identity_;
}

const NativePortDefinition& NativePortArtifact::definition() const noexcept {
    return definition_;
}

} // namespace katana::runtime
