#include "katana/runtime/native_bringup_artifact.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::array<std::uint8_t, 8u> artifact_magic{
    'K', 'A', 'T', 'N', 'B', 'A', '1', '\n'};
constexpr std::uint32_t artifact_header_size = 88u;
constexpr std::uint32_t artifact_maximum_targets = 4096u;
constexpr std::uint32_t artifact_maximum_string_size = 4096u;
constexpr std::uint32_t artifact_maximum_identity_size = 128u;
constexpr std::size_t artifact_sha256_size = 64u;

[[noreturn]] void artifact_error(const std::string_view message) {
    throw std::runtime_error(std::string(message));
}

[[nodiscard]] bool lower_hex(const std::string_view value) noexcept {
    return std::ranges::all_of(value, [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool valid_sha256(const std::string_view value) noexcept {
    constexpr std::string_view prefix = "sha256:";
    return value.starts_with(prefix) &&
           value.size() == prefix.size() + artifact_sha256_size &&
           lower_hex(value.substr(prefix.size()));
}

[[nodiscard]] bool valid_component(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 128u) return false;
    return std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               character == '-' || character == '_' || character == '.';
    });
}

[[nodiscard]] bool valid_text(const std::string_view value,
                              const bool required) noexcept {
    if ((required && value.empty()) ||
        value.size() > artifact_maximum_string_size)
        return false;
    return std::ranges::all_of(value, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20u && byte <= 0x7Eu;
    });
}

[[nodiscard]] bool valid_stage(const NativeBringupEvidenceStage value) noexcept {
    switch (value) {
    case NativeBringupEvidenceStage::Observed:
    case NativeBringupEvidenceStage::Candidate:
    case NativeBringupEvidenceStage::Proven:
    case NativeBringupEvidenceStage::RuntimeContract:
    case NativeBringupEvidenceStage::Unresolved:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_transfer_kind(
    const NativeBringupTransferKind value) noexcept {
    return value == NativeBringupTransferKind::CallRegister ||
           value == NativeBringupTransferKind::TailJumpRegister;
}

[[nodiscard]] bool valid_promotion_type(
    const NativeBringupPromotionType value) noexcept {
    switch (value) {
    case NativeBringupPromotionType::None:
    case NativeBringupPromotionType::StaticCompiledTarget:
    case NativeBringupPromotionType::ValidatedRuntimeContract:
    case NativeBringupPromotionType::AnalyzerReproof:
        return true;
    }
    return false;
}

[[nodiscard]] bool range_contains(const std::uint32_t outer_start,
                                  const std::uint32_t outer_size,
                                  const std::uint32_t inner_start,
                                  const std::uint32_t inner_size) noexcept {
    const auto outer_end =
        static_cast<std::uint64_t>(outer_start) + outer_size;
    const auto inner_end =
        static_cast<std::uint64_t>(inner_start) + inner_size;
    return outer_size != 0u && inner_size != 0u &&
           outer_end <= 0x1'0000'0000ull &&
           inner_end <= 0x1'0000'0000ull &&
           inner_start >= outer_start && inner_end <= outer_end;
}

[[nodiscard]] std::string hash_bytes(
    const std::span<const std::uint8_t> bytes) {
    const auto view = bytes.empty()
                          ? std::string_view{}
                          : std::string_view(
                                reinterpret_cast<const char*>(bytes.data()),
                                bytes.size());
    return io::sha256_bytes(view);
}

class BinaryWriter final {
  public:
    explicit BinaryWriter(const std::size_t maximum_size)
        : maximum_size_(maximum_size) {}

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

    template <typename Enum>
    void enumeration(const Enum value) {
        static_assert(std::is_enum_v<Enum>);
        u32(static_cast<std::uint32_t>(value));
    }

    void string(const std::string_view value,
                const std::uint32_t maximum_size =
                    artifact_maximum_string_size) {
        if (value.size() > maximum_size)
            artifact_error("Native bring-up artifact string is too long.");
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
        if (bytes_.size() > maximum_size_ ||
            size > maximum_size_ - bytes_.size())
            artifact_error(
                "Native bring-up artifact exceeds its hard size limit.");
    }

    std::size_t maximum_size_;
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader final {
  public:
    explicit BinaryReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}

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

    template <typename Enum>
    [[nodiscard]] Enum enumeration() {
        static_assert(std::is_enum_v<Enum>);
        using Underlying = std::underlying_type_t<Enum>;
        using Unsigned = std::make_unsigned_t<Underlying>;
        const auto value = u32();
        if (value > static_cast<std::uint32_t>(
                        std::numeric_limits<Unsigned>::max()))
            artifact_error("Native bring-up artifact enum is invalid.");
        return static_cast<Enum>(static_cast<Underlying>(
            static_cast<Unsigned>(value)));
    }

    [[nodiscard]] std::string string(
        const std::uint32_t maximum_size = artifact_maximum_string_size) {
        const auto size = u32();
        if (size > maximum_size)
            artifact_error("Native bring-up artifact string is too long.");
        const auto bytes = take(size);
        return std::string(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
    }

    [[nodiscard]] std::span<const std::uint8_t> take(
        const std::size_t size) {
        if (position_ > bytes_.size() || size > bytes_.size() - position_)
            artifact_error("Native bring-up artifact is truncated.");
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - position_;
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0u;
};

void serialize_target(BinaryWriter& writer,
                      const NativeBringupTargetEvidence& target) {
    writer.u32(target.contract_version);
    writer.enumeration(target.stage);
    writer.enumeration(target.transfer_kind);
    writer.u32(target.source_owner);
    writer.u32(target.source_owner_size);
    writer.u32(target.source_block);
    writer.u32(target.source_block_size);
    writer.u32(target.callsite);
    writer.u32(target.continuation);
    writer.string(target.source_owner_code_identity,
                  artifact_maximum_identity_size);
    writer.string(target.source_block_code_identity,
                  artifact_maximum_identity_size);
    writer.string(target.callsite_code_identity,
                  artifact_maximum_identity_size);
    writer.u32(target.target);
    writer.u32(target.target_block_size);
    writer.u32(target.target_owner);
    writer.u32(target.target_owner_size);
    writer.string(target.target_block_code_identity,
                  artifact_maximum_identity_size);
    writer.string(target.target_owner_code_identity,
                  artifact_maximum_identity_size);
    writer.string(target.source_image_id, artifact_maximum_identity_size);
    writer.string(target.target_image_id, artifact_maximum_identity_size);
    writer.string(target.source_module_identity,
                  artifact_maximum_identity_size);
    writer.string(target.target_module_identity,
                  artifact_maximum_identity_size);
    writer.u64(target.source_generation);
    writer.u64(target.target_generation);
    writer.string(target.observation);
    writer.string(target.static_correlation);
    writer.string(target.missing_proof);
    writer.enumeration(target.proposed_promotion);
    writer.string(target.analyzer_path);
    writer.string(target.runtime_contract_identity,
                  artifact_maximum_identity_size);
}

struct DecodedTarget final {
    NativeBringupTargetEvidence value;
    std::string source_owner_code_identity;
    std::string source_block_code_identity;
    std::string callsite_code_identity;
    std::string target_block_code_identity;
    std::string target_owner_code_identity;
    std::string source_image_id;
    std::string target_image_id;
    std::string source_module_identity;
    std::string target_module_identity;
    std::string observation;
    std::string static_correlation;
    std::string missing_proof;
    std::string analyzer_path;
    std::string runtime_contract_identity;
};

[[nodiscard]] DecodedTarget read_target(BinaryReader& reader) {
    DecodedTarget storage;
    auto& value = storage.value;
    value.contract_version = reader.u32();
    value.stage = reader.enumeration<NativeBringupEvidenceStage>();
    value.transfer_kind = reader.enumeration<NativeBringupTransferKind>();
    value.source_owner = reader.u32();
    value.source_owner_size = reader.u32();
    value.source_block = reader.u32();
    value.source_block_size = reader.u32();
    value.callsite = reader.u32();
    value.continuation = reader.u32();
    storage.source_owner_code_identity =
        reader.string(artifact_maximum_identity_size);
    storage.source_block_code_identity =
        reader.string(artifact_maximum_identity_size);
    storage.callsite_code_identity =
        reader.string(artifact_maximum_identity_size);
    value.target = reader.u32();
    value.target_block_size = reader.u32();
    value.target_owner = reader.u32();
    value.target_owner_size = reader.u32();
    storage.target_block_code_identity =
        reader.string(artifact_maximum_identity_size);
    storage.target_owner_code_identity =
        reader.string(artifact_maximum_identity_size);
    storage.source_image_id = reader.string(artifact_maximum_identity_size);
    storage.target_image_id = reader.string(artifact_maximum_identity_size);
    storage.source_module_identity =
        reader.string(artifact_maximum_identity_size);
    storage.target_module_identity =
        reader.string(artifact_maximum_identity_size);
    value.source_generation = reader.u64();
    value.target_generation = reader.u64();
    storage.observation = reader.string();
    storage.static_correlation = reader.string();
    storage.missing_proof = reader.string();
    value.proposed_promotion =
        reader.enumeration<NativeBringupPromotionType>();
    storage.analyzer_path = reader.string();
    storage.runtime_contract_identity =
        reader.string(artifact_maximum_identity_size);
    return storage;
}

[[nodiscard]] std::vector<std::uint8_t> serialize_definition(
    const NativeBringupAuthoringDefinition& definition) {
    validate_native_bringup_authoring_definition(definition);
    BinaryWriter writer(native_bringup_artifact_maximum_size -
                        artifact_header_size);
    writer.u32(definition.contract_version);
    writer.string(definition.project_id, artifact_maximum_identity_size);
    writer.string(definition.project_version, artifact_maximum_identity_size);
    writer.string(definition.analysis_identity,
                  artifact_maximum_identity_size);
    writer.string(definition.aot_pack_identity,
                  artifact_maximum_identity_size);
    writer.u64(definition.aot_pack_generation);
    std::vector<const NativeBringupTargetEvidence*> ordered_targets;
    ordered_targets.reserve(definition.targets.size());
    for (const auto& target : definition.targets)
        ordered_targets.push_back(&target);
    std::ranges::sort(
        ordered_targets, [](const auto* left, const auto* right) {
            return std::tuple{static_cast<std::uint32_t>(left->transfer_kind),
                              left->callsite,
                              left->target} <
                   std::tuple{static_cast<std::uint32_t>(right->transfer_kind),
                              right->callsite,
                              right->target};
        });
    writer.u32(static_cast<std::uint32_t>(ordered_targets.size()));
    for (const auto* target : ordered_targets)
        serialize_target(writer, *target);
    return std::move(writer).finish();
}

[[nodiscard]] std::vector<std::uint8_t> encode_artifact(
    const NativeBringupAuthoringDefinition& definition) {
    const auto payload = serialize_definition(definition);
    const auto payload_sha256 = hash_bytes(payload);
    BinaryWriter writer(native_bringup_artifact_maximum_size);
    writer.raw(std::span<const std::uint8_t>(artifact_magic));
    writer.u32(native_bringup_artifact_format_version);
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
            "Native bring-up artifact must be a regular non-symlink file.");
}

[[nodiscard]] std::filesystem::path canonical_regular_file(
    const std::filesystem::path& path) {
    if (path.empty()) artifact_error("Native bring-up artifact path is empty.");
    require_regular_nonsymlink_file(path);
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error)
        artifact_error(
            "Native bring-up artifact path cannot be canonicalized.");
    require_regular_nonsymlink_file(canonical);
    return canonical;
}

[[nodiscard]] std::vector<std::uint8_t> read_artifact_file(
    const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        artifact_error("Native bring-up artifact cannot be opened.");
    try {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        LARGE_INTEGER size{};
        if (::GetFileType(handle) != FILE_TYPE_DISK ||
            !::GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                            &attributes,
                                            sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
            !::GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) >
                native_bringup_artifact_maximum_size)
            artifact_error(
                "Native bring-up artifact is not a bounded regular file.");
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0u;
        while (offset != bytes.size()) {
            DWORD read = 0u;
            const auto remaining = bytes.size() - offset;
            const auto request = static_cast<DWORD>(std::min<std::size_t>(
                remaining, std::numeric_limits<DWORD>::max()));
            if (!::ReadFile(handle, bytes.data() + offset, request, &read,
                            nullptr) ||
                read == 0u)
                artifact_error(
                    "Native bring-up artifact cannot be read.");
            offset += read;
        }
        LARGE_INTEGER final_size{};
        if (!::GetFileSizeEx(handle, &final_size) ||
            final_size.QuadPart != size.QuadPart)
            artifact_error(
                "Native bring-up artifact changed while it was read.");
        if (!::CloseHandle(handle))
            artifact_error(
                "Native bring-up artifact handle cannot be closed.");
        return bytes;
    } catch (...) {
        static_cast<void>(::CloseHandle(handle));
        throw;
    }
#else
    auto flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0)
        artifact_error("Native bring-up artifact cannot be opened.");
    try {
        struct stat before {};
        if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
            before.st_size < 0 ||
            static_cast<std::uint64_t>(before.st_size) >
                native_bringup_artifact_maximum_size)
            artifact_error(
                "Native bring-up artifact is not a bounded regular file.");
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(before.st_size));
        std::size_t offset = 0u;
        while (offset != bytes.size()) {
            const auto read =
                ::read(descriptor, bytes.data() + offset,
                       bytes.size() - offset);
            if (read < 0 && errno == EINTR) continue;
            if (read <= 0)
                artifact_error(
                    "Native bring-up artifact cannot be read.");
            offset += static_cast<std::size_t>(read);
        }
        struct stat after {};
        if (::fstat(descriptor, &after) != 0 ||
            before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_size != after.st_size ||
            before.st_mtime != after.st_mtime ||
            before.st_ctime != after.st_ctime)
            artifact_error(
                "Native bring-up artifact changed while it was read.");
        if (::close(descriptor) != 0)
            artifact_error(
                "Native bring-up artifact handle cannot be closed.");
        return bytes;
    } catch (...) {
        static_cast<void>(::close(descriptor));
        throw;
    }
#endif
}

[[nodiscard]] std::filesystem::path normalized_destination(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty() || path.filename() == "." ||
        path.filename() == "..")
        artifact_error("Native bring-up artifact destination is invalid.");
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(absolute.parent_path());
    std::error_code error;
    const auto parent =
        std::filesystem::canonical(absolute.parent_path(), error);
    if (error)
        artifact_error(
            "Native bring-up artifact destination cannot be canonicalized.");
    const auto parent_status = std::filesystem::symlink_status(parent, error);
    if (error || !std::filesystem::is_directory(parent_status) ||
        std::filesystem::is_symlink(parent_status))
        artifact_error(
            "Native bring-up artifact parent is not a canonical directory.");
    const auto result = parent / absolute.filename();
    const auto status = std::filesystem::symlink_status(result, error);
    const auto missing =
        (!error && status.type() == std::filesystem::file_type::not_found) ||
        error == std::errc::no_such_file_or_directory;
    if (!missing && !error &&
        (std::filesystem::is_symlink(status) ||
         !std::filesystem::is_regular_file(status)))
        artifact_error(
            "Native bring-up artifact destination is not a regular file.");
    if (!missing && error)
        artifact_error(
            "Native bring-up artifact destination cannot be inspected.");
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
                "Temporary native bring-up artifact path cannot be inspected.");
        if (std::filesystem::is_symlink(status))
            artifact_error(
                "Temporary native bring-up artifact path is a symlink.");
    }
    artifact_error("No temporary native bring-up artifact path is available.");
}

void durable_write(const std::filesystem::path& path,
                   const std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        artifact_error(
            "Temporary native bring-up artifact cannot be created.");
    try {
        std::size_t offset = 0u;
        while (offset != bytes.size()) {
            DWORD written = 0u;
            const auto remaining = bytes.size() - offset;
            const auto request = static_cast<DWORD>(std::min<std::size_t>(
                remaining, std::numeric_limits<DWORD>::max()));
            if (!::WriteFile(handle, bytes.data() + offset, request, &written,
                             nullptr) ||
                written == 0u)
                artifact_error(
                    "Temporary native bring-up artifact cannot be written.");
            offset += written;
        }
        if (!::FlushFileBuffers(handle))
            artifact_error(
                "Temporary native bring-up artifact cannot be synchronized.");
    } catch (...) {
        static_cast<void>(::CloseHandle(handle));
        throw;
    }
    if (!::CloseHandle(handle))
        artifact_error(
            "Temporary native bring-up artifact cannot be closed.");
#else
    auto flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const auto descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
        artifact_error(
            "Temporary native bring-up artifact cannot be created.");
    try {
        std::size_t offset = 0u;
        while (offset != bytes.size()) {
            const auto written =
                ::write(descriptor, bytes.data() + offset,
                        bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0)
                artifact_error(
                    "Temporary native bring-up artifact cannot be written.");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(descriptor) != 0)
            artifact_error(
                "Temporary native bring-up artifact cannot be synchronized.");
    } catch (...) {
        static_cast<void>(::close(descriptor));
        throw;
    }
    if (::close(descriptor) != 0)
        artifact_error(
            "Temporary native bring-up artifact cannot be closed.");
#endif
}

void atomic_replace(const std::filesystem::path& source,
                    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!::MoveFileExW(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        artifact_error(
            "Native bring-up artifact cannot be atomically published.");
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error)
        artifact_error(
            "Native bring-up artifact cannot be atomically published.");
    auto flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const auto parent = ::open(destination.parent_path().c_str(), flags);
    if (parent < 0)
        artifact_error(
            "Native bring-up artifact directory cannot be synchronized.");
    const auto synchronized = ::fsync(parent) == 0;
    const auto closed = ::close(parent) == 0;
    if (!synchronized || !closed)
        artifact_error(
            "Native bring-up artifact directory cannot be synchronized.");
#endif
}

} // namespace

bool native_bringup_stage_is_static_proof(
    const NativeBringupEvidenceStage stage) noexcept {
    return stage == NativeBringupEvidenceStage::Proven;
}

void validate_native_bringup_authoring_definition(
    const NativeBringupAuthoringDefinition& definition) {
    if (definition.contract_version !=
            native_bringup_evidence_contract_version ||
        !valid_component(definition.project_id) ||
        !valid_component(definition.project_version) ||
        !valid_sha256(definition.analysis_identity) ||
        !valid_sha256(definition.aot_pack_identity) ||
        definition.aot_pack_generation == 0u ||
        definition.targets.size() > artifact_maximum_targets)
        artifact_error("Native bring-up authoring definition is invalid.");

    std::set<std::pair<std::uint32_t, std::uint32_t>> unique_targets;
    using SourceContract =
        std::tuple<std::uint32_t,
                   std::uint32_t,
                   std::uint32_t,
                   std::uint32_t,
                   NativeBringupTransferKind,
                   std::uint32_t,
                   std::string_view,
                   std::string_view,
                   std::string_view,
                   std::string_view,
                   std::string_view,
                   std::uint64_t>;
    std::map<std::uint32_t,
             SourceContract>
        source_contracts;
    for (const auto& target : definition.targets) {
        if (target.contract_version !=
                native_bringup_evidence_contract_version ||
            !valid_stage(target.stage) ||
            !valid_transfer_kind(target.transfer_kind) ||
            !valid_promotion_type(target.proposed_promotion) ||
            target.callsite == 0u || (target.callsite & 1u) != 0u ||
            (target.target & 1u) != 0u ||
            !valid_text(target.observation, true) ||
            !valid_text(target.static_correlation, true) ||
            !valid_text(target.missing_proof, false) ||
            !valid_text(target.analyzer_path, true) ||
            !unique_targets.emplace(target.callsite, target.target).second)
            artifact_error("Native bring-up target evidence is invalid.");

        const auto source_contract = std::tuple{
            target.source_owner,
            target.source_owner_size,
            target.source_block,
            target.source_block_size,
            target.transfer_kind,
            target.continuation,
            target.source_owner_code_identity,
            target.source_block_code_identity,
            target.callsite_code_identity,
            target.source_image_id,
            target.source_module_identity,
            target.source_generation};
        const auto [source, inserted] =
            source_contracts.emplace(target.callsite, source_contract);
        if (!inserted && source->second != source_contract)
            artifact_error(
                "Native bring-up callsite has conflicting source authority.");

        if (target.stage == NativeBringupEvidenceStage::Candidate ||
            target.stage == NativeBringupEvidenceStage::Proven) {
            const auto valid_continuation =
                target.transfer_kind ==
                        NativeBringupTransferKind::CallRegister
                    ? static_cast<std::uint64_t>(target.continuation) ==
                          static_cast<std::uint64_t>(target.callsite) + 4u
                    : target.continuation == 0u;
            const auto terminal_end =
                static_cast<std::uint64_t>(target.callsite) + 4u;
            const auto source_block_end =
                static_cast<std::uint64_t>(target.source_block) +
                target.source_block_size;
            if (!range_contains(target.source_owner,
                                target.source_owner_size,
                                target.source_block,
                                target.source_block_size) ||
                !range_contains(target.source_block,
                                target.source_block_size,
                                target.callsite,
                                4u) ||
                terminal_end != source_block_end ||
                !range_contains(target.target_owner,
                                target.target_owner_size,
                                target.target,
                                target.target_block_size) ||
                (target.source_owner & 1u) != 0u ||
                (target.source_owner_size & 1u) != 0u ||
                (target.source_block & 1u) != 0u ||
                target.source_block_size < 4u ||
                (target.source_block_size & 1u) != 0u ||
                (target.target_owner & 1u) != 0u ||
                (target.target_owner_size & 1u) != 0u ||
                (target.target_block_size & 1u) != 0u ||
                !valid_continuation ||
                !valid_sha256(target.source_owner_code_identity) ||
                !valid_sha256(target.source_block_code_identity) ||
                !valid_sha256(target.callsite_code_identity) ||
                !valid_sha256(target.target_block_code_identity) ||
                !valid_sha256(target.target_owner_code_identity) ||
                !valid_component(target.source_image_id) ||
                !valid_component(target.target_image_id) ||
                !valid_sha256(target.source_module_identity) ||
                !valid_sha256(target.target_module_identity) ||
                target.source_generation !=
                    definition.aot_pack_generation ||
                target.target_generation !=
                    definition.aot_pack_generation ||
                !target.runtime_contract_identity.empty())
                artifact_error(
                    "Executable native bring-up target lacks exact safety "
                    "evidence.");
            if (target.stage == NativeBringupEvidenceStage::Proven) {
                if (!target.missing_proof.empty() ||
                    target.proposed_promotion !=
                        NativeBringupPromotionType::StaticCompiledTarget)
                    artifact_error(
                        "Proven native bring-up target lacks exact static "
                        "proof.");
            } else if (target.missing_proof.empty() ||
                       target.proposed_promotion !=
                           NativeBringupPromotionType::AnalyzerReproof ||
                       target.static_correlation.empty() ||
                       target.analyzer_path.empty()) {
                artifact_error(
                    "Candidate native bring-up target lacks an open proof "
                    "task.");
            }
        } else if (target.stage ==
                   NativeBringupEvidenceStage::RuntimeContract) {
            if (!valid_sha256(target.runtime_contract_identity) ||
                target.proposed_promotion !=
                    NativeBringupPromotionType::ValidatedRuntimeContract ||
                !target.missing_proof.empty())
                artifact_error(
                    "Native bring-up runtime contract is not identity-bound.");
        } else {
            if (target.missing_proof.empty() ||
                target.proposed_promotion == NativeBringupPromotionType::None ||
                target.static_correlation.empty() ||
                target.analyzer_path.empty())
                artifact_error(
                    "Incomplete native bring-up evidence lacks a promotion task.");
        }
    }
}

void NativeBringupAuthoringArtifact::rebuild_definition() {
    targets_.clear();
    targets_.reserve(target_storage_.size());
    for (auto& storage : target_storage_) {
        auto value = storage.value;
        value.source_owner_code_identity =
            storage.source_owner_code_identity;
        value.source_block_code_identity =
            storage.source_block_code_identity;
        value.callsite_code_identity = storage.callsite_code_identity;
        value.target_block_code_identity =
            storage.target_block_code_identity;
        value.target_owner_code_identity =
            storage.target_owner_code_identity;
        value.source_image_id = storage.source_image_id;
        value.target_image_id = storage.target_image_id;
        value.source_module_identity = storage.source_module_identity;
        value.target_module_identity = storage.target_module_identity;
        value.observation = storage.observation;
        value.static_correlation = storage.static_correlation;
        value.missing_proof = storage.missing_proof;
        value.analyzer_path = storage.analyzer_path;
        value.runtime_contract_identity = storage.runtime_contract_identity;
        targets_.push_back(value);
    }
    definition_ = {native_bringup_evidence_contract_version,
                   project_id_,
                   project_version_,
                   analysis_identity_,
                   aot_pack_identity_,
                   aot_pack_generation_,
                   targets_};
    validate_native_bringup_authoring_definition(definition_);
}

std::shared_ptr<NativeBringupAuthoringArtifact>
NativeBringupAuthoringArtifact::load(const std::filesystem::path& path) {
    const auto canonical = canonical_regular_file(path);
    const auto bytes = read_artifact_file(canonical);
    BinaryReader reader(bytes);
    if (!std::ranges::equal(reader.take(artifact_magic.size()), artifact_magic))
        artifact_error("Native bring-up artifact magic is invalid.");
    if (reader.u32() != native_bringup_artifact_format_version ||
        reader.u32() != artifact_header_size)
        artifact_error("Native bring-up artifact format is unsupported.");
    const auto payload_size = reader.u64();
    const auto payload_sha = reader.take(artifact_sha256_size);
    if (payload_size != reader.remaining())
        artifact_error("Native bring-up artifact payload size is invalid.");
    const auto payload = reader.take(static_cast<std::size_t>(payload_size));
    if (hash_bytes(payload) !=
        std::string(reinterpret_cast<const char*>(payload_sha.data()),
                    payload_sha.size()))
        artifact_error("Native bring-up artifact payload identity is invalid.");

    BinaryReader payload_reader(payload);
    auto result = std::shared_ptr<NativeBringupAuthoringArtifact>(
        new NativeBringupAuthoringArtifact);
    result->canonical_path_ = canonical;
    result->artifact_identity_ = "sha256:" + hash_bytes(bytes);
    const auto contract_version = payload_reader.u32();
    if (contract_version != native_bringup_evidence_contract_version)
        artifact_error("Native bring-up evidence contract is unsupported.");
    result->project_id_ =
        payload_reader.string(artifact_maximum_identity_size);
    result->project_version_ =
        payload_reader.string(artifact_maximum_identity_size);
    result->analysis_identity_ =
        payload_reader.string(artifact_maximum_identity_size);
    result->aot_pack_identity_ =
        payload_reader.string(artifact_maximum_identity_size);
    result->aot_pack_generation_ = payload_reader.u64();
    const auto target_count = payload_reader.u32();
    if (target_count > artifact_maximum_targets)
        artifact_error("Native bring-up target count exceeds its limit.");
    result->target_storage_.reserve(target_count);
    for (std::uint32_t index = 0u; index < target_count; ++index) {
        auto decoded = read_target(payload_reader);
        TargetStorage storage;
        storage.value = decoded.value;
        storage.source_owner_code_identity =
            std::move(decoded.source_owner_code_identity);
        storage.source_block_code_identity =
            std::move(decoded.source_block_code_identity);
        storage.callsite_code_identity =
            std::move(decoded.callsite_code_identity);
        storage.target_block_code_identity =
            std::move(decoded.target_block_code_identity);
        storage.target_owner_code_identity =
            std::move(decoded.target_owner_code_identity);
        storage.source_image_id = std::move(decoded.source_image_id);
        storage.target_image_id = std::move(decoded.target_image_id);
        storage.source_module_identity =
            std::move(decoded.source_module_identity);
        storage.target_module_identity =
            std::move(decoded.target_module_identity);
        storage.observation = std::move(decoded.observation);
        storage.static_correlation =
            std::move(decoded.static_correlation);
        storage.missing_proof = std::move(decoded.missing_proof);
        storage.analyzer_path = std::move(decoded.analyzer_path);
        storage.runtime_contract_identity =
            std::move(decoded.runtime_contract_identity);
        result->target_storage_.push_back(std::move(storage));
    }
    if (!std::ranges::is_sorted(
            result->target_storage_, [](const auto& left, const auto& right) {
                return std::tuple{
                           static_cast<std::uint32_t>(
                               left.value.transfer_kind),
                           left.value.callsite,
                           left.value.target} <
                       std::tuple{
                           static_cast<std::uint32_t>(
                               right.value.transfer_kind),
                           right.value.callsite,
                           right.value.target};
            }))
        artifact_error(
            "Native bring-up artifact target order is not canonical.");
    if (payload_reader.remaining() != 0u)
        artifact_error("Native bring-up artifact has trailing bytes.");
    result->rebuild_definition();
    return result;
}

std::shared_ptr<NativeBringupAuthoringArtifact>
NativeBringupAuthoringArtifact::write(
    const std::filesystem::path& path,
    const NativeBringupAuthoringDefinition& definition) {
    validate_native_bringup_authoring_definition(definition);
    const auto destination = normalized_destination(path);
    const auto bytes = encode_artifact(definition);
    const auto temporary = temporary_path(destination);
    try {
        durable_write(temporary, bytes);
        atomic_replace(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    auto result = load(destination);
    if (result->artifact_identity_ != "sha256:" + hash_bytes(bytes))
        artifact_error(
            "Published native bring-up artifact identity changed.");
    return result;
}

const std::filesystem::path&
NativeBringupAuthoringArtifact::canonical_path() const noexcept {
    return canonical_path_;
}

const std::string&
NativeBringupAuthoringArtifact::artifact_identity() const noexcept {
    return artifact_identity_;
}

const NativeBringupAuthoringDefinition&
NativeBringupAuthoringArtifact::definition() const noexcept {
    return definition_;
}

} // namespace katana::runtime
