#include "katana/runtime/game_project_artifact.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
    'K', 'A', 'T', 'G', 'P', 'R', '1', '\n'};
constexpr std::uint32_t artifact_header_size = 88u;
constexpr std::uint32_t artifact_maximum_entries = 1'000'000u;
constexpr std::uint32_t artifact_maximum_templates = 65'536u;
constexpr std::uint32_t artifact_maximum_string_size = 4096u;
constexpr std::uint32_t artifact_maximum_identity_size = 128u;
constexpr std::uint32_t artifact_maximum_runtime_image_size =
    16u * 1024u * 1024u;
constexpr std::uint32_t artifact_maximum_runtime_image_total_size =
    64u * 1024u * 1024u;
constexpr std::uint32_t artifact_maximum_runtime_image_entries =
    65'536u;
constexpr std::size_t artifact_sha256_size = 64u;
constexpr std::string_view sha256_prefix = "sha256:";

[[noreturn]] void artifact_error(const std::string_view message) {
    throw std::runtime_error(std::string(message));
}

bool lower_hex(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

std::string hash_bytes(const std::span<const std::uint8_t> bytes) {
    const auto text =
        bytes.empty()
            ? std::string_view{}
            : std::string_view(
                  reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return io::sha256_bytes(text);
}

class BinaryWriter final {
  public:
    void u8(const std::uint8_t value) {
        bytes_.push_back(value);
    }

    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void i32(const std::int32_t value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }

    void u64(const std::uint64_t value) {
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
            artifact_error("Game-project artifact string is too long.");
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value);
    }

    void raw(const std::string_view value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void raw(const std::span<const std::uint8_t> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
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

    [[nodiscard]] std::int32_t i32() {
        return std::bit_cast<std::int32_t>(u32());
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
            artifact_error("Game-project artifact boolean is invalid.");
        return value != 0u;
    }

    template <typename Enum>
    [[nodiscard]] Enum enumeration() {
        static_assert(std::is_enum_v<Enum>);
        using Underlying = std::underlying_type_t<Enum>;
        using Unsigned = std::make_unsigned_t<Underlying>;
        const auto value = u32();
        if (value >
            static_cast<std::uint32_t>(
                std::numeric_limits<Unsigned>::max()))
            artifact_error(
                "Game-project artifact enum encoding is invalid.");
        return static_cast<Enum>(
            static_cast<Underlying>(static_cast<Unsigned>(value)));
    }

    [[nodiscard]] std::string string(
        const std::uint32_t maximum_size =
            artifact_maximum_string_size) {
        const auto size = u32();
        if (size > maximum_size)
            artifact_error("Game-project artifact string is too long.");
        const auto bytes = take(size);
        return std::string(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    [[nodiscard]] std::uint32_t count(
        const std::uint32_t maximum = artifact_maximum_entries) {
        const auto value = u32();
        if (value > maximum)
            artifact_error("Game-project artifact count exceeds its limit.");
        return value;
    }

    [[nodiscard]] std::span<const std::uint8_t> take(
        const std::size_t size) {
        if (size > bytes_.size() - offset_)
            artifact_error("Game-project artifact is truncated.");
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

std::uint32_t checked_count(
    const std::size_t value,
    const std::uint32_t maximum = artifact_maximum_entries) {
    if (value > maximum)
        artifact_error("Game-project artifact count exceeds its limit.");
    return static_cast<std::uint32_t>(value);
}

void write_boot_config(BinaryWriter& writer,
                       const DreamcastRuntimeBootConfig& config) {
    writer.enumeration(config.firmware_mode);
    writer.enumeration(config.boot_path);
    writer.u32(config.post_bios_platform_contract_version);
    const auto& cpu = config.post_bios_cpu_state;
    writer.u32(cpu.contract_version);
    writer.u32(cpu.entry_point);
    writer.u32(cpu.stack_pointer);
    writer.u32(cpu.vector_base);
    writer.u32(cpu.status);
    writer.u32(cpu.fpscr);
    writer.u32(cpu.gbr);
    writer.u32(cpu.ssr);
    writer.u32(cpu.spc);
    writer.u32(cpu.sgr);
    writer.u32(cpu.dbr);
    writer.u32(cpu.pr);
    writer.string(
        config.executable_identity.content_identity,
        artifact_maximum_identity_size);
    writer.string(config.executable_identity.boot_file_name, 255u);
    writer.string(
        config.executable_identity.boot_byte_identity,
        artifact_maximum_identity_size);
}

DreamcastRuntimeBootConfig read_boot_config(BinaryReader& reader) {
    DreamcastRuntimeBootConfig config;
    config.firmware_mode =
        reader.enumeration<DreamcastRuntimeFirmwareMode>();
    config.boot_path = reader.enumeration<DreamcastRuntimeBootPath>();
    config.post_bios_platform_contract_version = reader.u32();
    auto& cpu = config.post_bios_cpu_state;
    cpu.contract_version = reader.u32();
    cpu.entry_point = reader.u32();
    cpu.stack_pointer = reader.u32();
    cpu.vector_base = reader.u32();
    cpu.status = reader.u32();
    cpu.fpscr = reader.u32();
    cpu.gbr = reader.u32();
    cpu.ssr = reader.u32();
    cpu.spc = reader.u32();
    cpu.sgr = reader.u32();
    cpu.dbr = reader.u32();
    cpu.pr = reader.u32();
    config.executable_identity.content_identity =
        reader.string(artifact_maximum_identity_size);
    config.executable_identity.boot_file_name = reader.string(255u);
    config.executable_identity.boot_byte_identity =
        reader.string(artifact_maximum_identity_size);
    return config;
}

std::vector<std::uint8_t>
serialize_definition(const GameProjectDefinition& definition) {
    BinaryWriter writer;
    writer.u32(definition.contract_version);
    writer.string(definition.project_id);
    writer.string(definition.project_version);
    writer.string(
        definition.identity.content_identity,
        artifact_maximum_identity_size);
    writer.string(definition.identity.boot_file_name, 255u);
    writer.string(
        definition.identity.boot_byte_identity,
        artifact_maximum_identity_size);
    writer.enumeration(definition.required_product_milestone);

    writer.u32(checked_count(definition.function_boundaries.size()));
    for (const auto& function : definition.function_boundaries) {
        writer.u32(function.start);
        writer.u32(function.size);
        writer.string(function.symbol);
    }

    writer.u32(checked_count(definition.jump_tables.size()));
    for (const auto& table : definition.jump_tables) {
        writer.u32(table.dispatch_address);
        writer.u32(table.table_address);
        writer.u32(table.entry_count);
        writer.u32(table.entry_stride);
        writer.u32(table.relative_base);
        writer.enumeration(table.encoding);
        writer.enumeration(table.transfer);
    }

    writer.u32(checked_count(definition.callback_tables.size()));
    for (const auto& table : definition.callback_tables) {
        writer.u32(table.table_address);
        writer.u32(table.entry_count);
        writer.u32(table.entry_stride);
        writer.u32(table.pointer_offset);
        writer.enumeration(table.transfer);
    }

    writer.u32(checked_count(
        definition.runtime_code_templates.size(),
        artifact_maximum_templates));
    for (const auto& native_template :
         definition.runtime_code_templates) {
        writer.string(native_template.source_module_id);
        writer.string(
            native_template.expected_source_identity,
            artifact_maximum_identity_size);
        writer.u32(native_template.source_start);
        writer.u32(native_template.extent);
        writer.i32(native_template.destination_vbr_delta);
        writer.enumeration(native_template.destination);
        writer.string(
            native_template.expected_runtime_content_identity,
            artifact_maximum_identity_size);
        writer.string(
            native_template.expected_runtime_byte_identity,
            artifact_maximum_identity_size);
        writer.u32(checked_count(native_template.patches.size()));
        for (const auto& patch : native_template.patches) {
            writer.u32(patch.source_offset);
            writer.u32(checked_count(patch.allowed_targets.size()));
            for (const auto& target : patch.allowed_targets) {
                writer.u32(target.live_value);
                writer.u32(target.block_address);
            }
        }
        writer.u32(checked_count(native_template.mutable_ranges.size()));
        for (const auto& range : native_template.mutable_ranges) {
            writer.u32(range.offset);
            writer.u32(range.size);
        }
    }

    writer.u32(checked_count(definition.runtime_images.size()));
    for (const auto& image : definition.runtime_images) {
        writer.string(image.image_id);
        writer.string(
            image.byte_identity,
            artifact_maximum_identity_size);
        writer.u32(image.source_start);
        writer.u32(image.runtime_start);
        writer.u32(checked_count(
            image.byte_size,
            artifact_maximum_runtime_image_size));
        writer.u32(checked_count(
            image.entry_offsets.size(),
            artifact_maximum_runtime_image_entries));
        for (const auto entry : image.entry_offsets)
            writer.u32(entry);
    }

    writer.u32(checked_count(definition.symbols.size()));
    for (const auto& symbol : definition.symbols) {
        writer.u32(symbol.address);
        writer.u32(symbol.size);
        writer.enumeration(symbol.kind);
        writer.string(symbol.name);
    }

    writer.u32(checked_count(definition.code_identities.size()));
    for (const auto& identity : definition.code_identities) {
        writer.u32(identity.address);
        writer.u32(identity.size);
        writer.string(
            identity.byte_identity,
            artifact_maximum_identity_size);
    }

    writer.boolean(definition.boot_config.has_value());
    if (definition.boot_config.has_value())
        write_boot_config(writer, *definition.boot_config);
    auto bytes = std::move(writer).finish();
    if (bytes.size() >
        game_project_artifact_maximum_size - artifact_header_size)
        artifact_error("Game-project artifact payload is too large.");
    return bytes;
}

std::vector<std::uint8_t>
encode_artifact(const GameProjectDefinition& definition) {
    const auto payload = serialize_definition(definition);
    const auto payload_sha256 = hash_bytes(payload);
    BinaryWriter writer;
    writer.raw(artifact_magic);
    writer.u32(game_project_artifact_format_version);
    writer.u32(artifact_header_size);
    writer.u64(payload.size());
    writer.raw(payload_sha256);
    writer.raw(payload);
    return std::move(writer).finish();
}

void require_regular_nonsymlink_file(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        artifact_error(
            "Game-project artifact must be a regular non-symlink file.");
}

std::filesystem::path canonical_regular_file(
    const std::filesystem::path& path) {
    if (path.empty())
        artifact_error("Game-project artifact path is empty.");
    require_regular_nonsymlink_file(path);
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error)
        artifact_error(
            "Game-project artifact path cannot be canonicalized.");
    require_regular_nonsymlink_file(canonical);
    return canonical;
}

std::vector<std::uint8_t> read_artifact_file(
    const std::filesystem::path& path) {
    require_regular_nonsymlink_file(path);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        artifact_error("Game-project artifact cannot be opened.");
    const auto end = input.tellg();
    if (end < 0 ||
        static_cast<std::uint64_t>(end) >
            game_project_artifact_maximum_size)
        artifact_error(
            "Game-project artifact exceeds its hard size limit.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    if (!input)
        artifact_error("Game-project artifact cannot be read.");
    require_regular_nonsymlink_file(path);
    std::error_code error;
    if (std::filesystem::file_size(path, error) != bytes.size() || error)
        artifact_error(
            "Game-project artifact changed while it was read.");
    return bytes;
}

std::filesystem::path normalized_destination(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty() ||
        path.filename() == "." || path.filename() == "..")
        artifact_error("Game-project artifact destination is invalid.");
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(absolute.parent_path());
    std::error_code error;
    const auto parent =
        std::filesystem::canonical(absolute.parent_path(), error);
    if (error)
        artifact_error(
            "Game-project artifact destination cannot be canonicalized.");
    const auto parent_status =
        std::filesystem::symlink_status(parent, error);
    if (error || !std::filesystem::is_directory(parent_status) ||
        std::filesystem::is_symlink(parent_status))
        artifact_error(
            "Game-project artifact parent is not a canonical directory.");
    const auto result = parent / absolute.filename();
    const auto status = std::filesystem::symlink_status(result, error);
    const auto missing =
        (!error &&
         status.type() == std::filesystem::file_type::not_found) ||
        error == std::errc::no_such_file_or_directory;
    if (!missing && !error &&
        (std::filesystem::is_symlink(status) ||
         !std::filesystem::is_regular_file(status)))
        artifact_error(
            "Game-project artifact destination is not a regular file.");
    if (!missing && error)
        artifact_error(
            "Game-project artifact destination cannot be inspected.");
    return result;
}

std::filesystem::path temporary_path(
    const std::filesystem::path& destination) {
    for (std::size_t attempt = 1u; attempt <= 1024u; ++attempt) {
        auto candidate = destination;
        candidate += ".tmp-" + std::to_string(attempt);
        std::error_code error;
        const auto status =
            std::filesystem::symlink_status(candidate, error);
        if ((!error &&
             status.type() == std::filesystem::file_type::not_found) ||
            error == std::errc::no_such_file_or_directory)
            return candidate;
        if (error)
            artifact_error(
                "Temporary game-project artifact path cannot be inspected.");
        if (std::filesystem::is_symlink(status))
            artifact_error(
                "Temporary game-project artifact path is a symlink.");
    }
    artifact_error(
        "No temporary game-project artifact path is available.");
}

void durable_write(const std::filesystem::path& path,
                   const std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    std::FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0)
        file = nullptr;
#else
    auto* file = std::fopen(path.c_str(), "wb");
#endif
    if (file == nullptr)
        artifact_error(
            "Temporary game-project artifact cannot be created.");
    try {
        const auto written =
            bytes.empty()
                ? 0u
                : std::fwrite(bytes.data(), 1u, bytes.size(), file);
        if (written != bytes.size() || std::fflush(file) != 0)
            artifact_error(
                "Temporary game-project artifact cannot be written.");
#ifdef _WIN32
        if (::_commit(::_fileno(file)) != 0)
#else
        if (::fsync(::fileno(file)) != 0)
#endif
            artifact_error(
                "Temporary game-project artifact cannot be synchronized.");
    } catch (...) {
        static_cast<void>(std::fclose(file));
        throw;
    }
    if (std::fclose(file) != 0)
        artifact_error(
            "Temporary game-project artifact cannot be closed.");
}

void atomic_replace(const std::filesystem::path& source,
                    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!::MoveFileExW(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        artifact_error(
            "Game-project artifact cannot be atomically published.");
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error)
        artifact_error(
            "Game-project artifact cannot be atomically published.");
#endif
}

} // namespace

void GameProjectArtifact::rebuild_definition() {
    if (function_symbols_.size() != function_boundaries_.size() ||
        symbol_names_.size() != symbols_.size() ||
        code_identity_values_.size() != code_identities_.size() ||
        runtime_image_ids_.size() != runtime_images_.size() ||
        runtime_image_byte_identities_.size() != runtime_images_.size() ||
        runtime_image_entry_offsets_.size() != runtime_images_.size())
        artifact_error("Game-project artifact ownership is inconsistent.");

    for (std::size_t index = 0u;
         index < function_boundaries_.size();
         ++index)
        function_boundaries_[index].symbol = function_symbols_[index];
    for (std::size_t index = 0u; index < symbols_.size(); ++index)
        symbols_[index].name = symbol_names_[index];
    for (std::size_t index = 0u;
         index < code_identities_.size();
         ++index)
        code_identities_[index].byte_identity =
            code_identity_values_[index];
    for (std::size_t index = 0u;
         index < runtime_images_.size();
         ++index) {
        runtime_images_[index].image_id = runtime_image_ids_[index];
        runtime_images_[index].byte_identity =
            runtime_image_byte_identities_[index];
        runtime_images_[index].entry_offsets =
            runtime_image_entry_offsets_[index];
    }

    definition_ = {};
    definition_.contract_version = contract_version_;
    definition_.project_id = project_id_;
    definition_.project_version = project_version_;
    definition_.identity = {
        content_identity_, boot_file_name_, boot_byte_identity_};
    definition_.required_product_milestone =
        required_product_milestone_;
    definition_.function_boundaries = function_boundaries_;
    definition_.jump_tables = jump_tables_;
    definition_.callback_tables = callback_tables_;
    definition_.runtime_code_templates = runtime_code_templates_;
    definition_.symbols = symbols_;
    definition_.code_identities = code_identities_;
    definition_.runtime_images = runtime_images_;
    definition_.boot_config = boot_config_;
    validate_game_project_definition(definition_);
}

std::shared_ptr<GameProjectArtifact>
GameProjectArtifact::load(const std::filesystem::path& path) {
    const auto canonical = canonical_regular_file(path);
    auto bytes = read_artifact_file(canonical);
    BinaryReader file_reader(bytes);
    const auto magic = file_reader.take(artifact_magic.size());
    if (!std::equal(
            magic.begin(), magic.end(), artifact_magic.begin()))
        artifact_error("Game-project artifact magic is invalid.");
    const auto format_version = file_reader.u32();
    const auto header_size = file_reader.u32();
    const auto payload_size = file_reader.u64();
    const auto payload_sha_bytes =
        file_reader.take(artifact_sha256_size);
    const std::string payload_sha256(
        reinterpret_cast<const char*>(payload_sha_bytes.data()),
        payload_sha_bytes.size());
    if (format_version != game_project_artifact_format_version ||
        header_size != artifact_header_size ||
        payload_size != bytes.size() - artifact_header_size ||
        !lower_hex(payload_sha256))
        artifact_error("Game-project artifact header is invalid.");
    const auto payload =
        file_reader.take(static_cast<std::size_t>(payload_size));
    if (!file_reader.empty() || hash_bytes(payload) != payload_sha256)
        artifact_error("Game-project artifact SHA-256 is invalid.");

    BinaryReader reader(payload);
    auto result =
        std::shared_ptr<GameProjectArtifact>(new GameProjectArtifact);
    result->canonical_path_ = canonical;
    result->artifact_identity_ =
        std::string(sha256_prefix) + hash_bytes(bytes);
    result->contract_version_ = reader.u32();
    result->project_id_ = reader.string();
    result->project_version_ = reader.string();
    result->content_identity_ =
        reader.string(artifact_maximum_identity_size);
    result->boot_file_name_ = reader.string(255u);
    result->boot_byte_identity_ =
        reader.string(artifact_maximum_identity_size);
    result->required_product_milestone_ =
        reader.enumeration<RequiredProductMilestone>();

    const auto function_count = reader.count();
    result->function_symbols_.reserve(function_count);
    result->function_boundaries_.reserve(function_count);
    for (std::uint32_t index = 0u; index < function_count; ++index) {
        GameProjectFunctionBoundary function;
        function.start = reader.u32();
        function.size = reader.u32();
        result->function_symbols_.push_back(reader.string());
        result->function_boundaries_.push_back(function);
    }

    const auto jump_table_count = reader.count();
    result->jump_tables_.reserve(jump_table_count);
    for (std::uint32_t index = 0u;
         index < jump_table_count;
         ++index) {
        GameProjectJumpTable table;
        table.dispatch_address = reader.u32();
        table.table_address = reader.u32();
        table.entry_count = reader.u32();
        table.entry_stride = reader.u32();
        table.relative_base = reader.u32();
        table.encoding =
            reader.enumeration<GameProjectTableEncoding>();
        table.transfer =
            reader.enumeration<GameProjectControlTransferKind>();
        result->jump_tables_.push_back(table);
    }

    const auto callback_table_count = reader.count();
    result->callback_tables_.reserve(callback_table_count);
    for (std::uint32_t index = 0u;
         index < callback_table_count;
         ++index) {
        GameProjectCallbackTable table;
        table.table_address = reader.u32();
        table.entry_count = reader.u32();
        table.entry_stride = reader.u32();
        table.pointer_offset = reader.u32();
        table.transfer =
            reader.enumeration<GameProjectControlTransferKind>();
        result->callback_tables_.push_back(table);
    }

    const auto template_count =
        reader.count(artifact_maximum_templates);
    result->runtime_code_templates_.reserve(template_count);
    for (std::uint32_t index = 0u; index < template_count; ++index) {
        NativeAotTemplate native_template;
        native_template.source_module_id = reader.string();
        native_template.expected_source_identity =
            reader.string(artifact_maximum_identity_size);
        native_template.source_start = reader.u32();
        native_template.extent = reader.u32();
        native_template.destination_vbr_delta = reader.i32();
        native_template.destination =
            reader.enumeration<NativeAotTemplateDestination>();
        native_template.expected_runtime_content_identity =
            reader.string(artifact_maximum_identity_size);
        native_template.expected_runtime_byte_identity =
            reader.string(artifact_maximum_identity_size);
        const auto patch_count = reader.count();
        native_template.patches.reserve(patch_count);
        for (std::uint32_t patch_index = 0u;
             patch_index < patch_count;
             ++patch_index) {
            NativeAotTemplatePatch patch;
            patch.source_offset = reader.u32();
            const auto target_count = reader.count();
            patch.allowed_targets.reserve(target_count);
            for (std::uint32_t target_index = 0u;
                 target_index < target_count;
                 ++target_index)
                patch.allowed_targets.push_back(
                    {reader.u32(), reader.u32()});
            native_template.patches.push_back(std::move(patch));
        }
        const auto mutable_range_count = reader.count();
        native_template.mutable_ranges.reserve(mutable_range_count);
        for (std::uint32_t range_index = 0u;
             range_index < mutable_range_count;
             ++range_index)
            native_template.mutable_ranges.push_back(
                {reader.u32(), reader.u32()});
        result->runtime_code_templates_.push_back(
            std::move(native_template));
    }

    const auto runtime_image_count = reader.count();
    result->runtime_image_ids_.reserve(runtime_image_count);
    result->runtime_image_byte_identities_.reserve(
        runtime_image_count);
    result->runtime_image_entry_offsets_.reserve(
        runtime_image_count);
    result->runtime_images_.reserve(runtime_image_count);
    std::uint32_t runtime_image_total_size = 0u;
    for (std::uint32_t index = 0u;
         index < runtime_image_count;
         ++index) {
        GameProjectRuntimeImage image;
        result->runtime_image_ids_.push_back(reader.string());
        result->runtime_image_byte_identities_.push_back(
            reader.string(artifact_maximum_identity_size));
        image.source_start = reader.u32();
        image.runtime_start = reader.u32();
        image.byte_size =
            reader.count(artifact_maximum_runtime_image_size);
        if (image.byte_size >
            artifact_maximum_runtime_image_total_size -
                runtime_image_total_size)
            artifact_error(
                "Game-project artifact runtime images exceed their total size limit.");
        runtime_image_total_size += image.byte_size;
        const auto entry_count =
            reader.count(artifact_maximum_runtime_image_entries);
        auto& entry_offsets =
            result->runtime_image_entry_offsets_.emplace_back();
        entry_offsets.reserve(entry_count);
        for (std::uint32_t entry_index = 0u;
             entry_index < entry_count;
             ++entry_index)
            entry_offsets.push_back(reader.u32());
        result->runtime_images_.push_back(image);
    }

    const auto symbol_count = reader.count();
    result->symbol_names_.reserve(symbol_count);
    result->symbols_.reserve(symbol_count);
    for (std::uint32_t index = 0u; index < symbol_count; ++index) {
        GameProjectSymbol symbol;
        symbol.address = reader.u32();
        symbol.size = reader.u32();
        symbol.kind = reader.enumeration<GameProjectSymbolKind>();
        result->symbol_names_.push_back(reader.string());
        result->symbols_.push_back(symbol);
    }

    const auto code_identity_count = reader.count();
    result->code_identity_values_.reserve(code_identity_count);
    result->code_identities_.reserve(code_identity_count);
    for (std::uint32_t index = 0u;
         index < code_identity_count;
         ++index) {
        GameProjectCodeIdentity identity;
        identity.address = reader.u32();
        identity.size = reader.u32();
        result->code_identity_values_.push_back(
            reader.string(artifact_maximum_identity_size));
        result->code_identities_.push_back(identity);
    }

    if (reader.boolean())
        result->boot_config_ = read_boot_config(reader);
    if (!reader.empty())
        artifact_error(
            "Game-project artifact has unexpected trailing data.");
    result->rebuild_definition();
    return result;
}

std::shared_ptr<GameProjectArtifact>
GameProjectArtifact::write(
    const std::filesystem::path& path,
    const GameProjectDefinition& definition) {
    if (!definition.function_overrides.empty() ||
        !definition.mid_function_hooks.empty())
        throw std::invalid_argument(
            "game-project-artifact-native-hooks-not-serializable");
    if (definition.game_entry_handoff.has_value())
        throw std::invalid_argument(
            "game-project-artifact-game-entry-handoff-not-serializable");
    validate_game_project_definition(definition);
    const auto bytes = encode_artifact(definition);
    const auto expected_artifact_identity =
        std::string(sha256_prefix) + hash_bytes(bytes);
    const auto expected_definition_identity =
        game_project_definition_identity(definition);
    const auto destination = normalized_destination(path);
    const auto temporary = temporary_path(destination);
    try {
        durable_write(temporary, bytes);
        const auto reread = load(temporary);
        if (reread->artifact_identity() != expected_artifact_identity ||
            game_project_definition_identity(reread->definition()) !=
                expected_definition_identity)
            artifact_error(
                "Temporary game-project artifact validation changed its content.");
        atomic_replace(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return load(destination);
}

const std::filesystem::path&
GameProjectArtifact::canonical_path() const noexcept {
    return canonical_path_;
}

const std::string&
GameProjectArtifact::artifact_identity() const noexcept {
    return artifact_identity_;
}

const GameProjectDefinition&
GameProjectArtifact::definition() const noexcept {
    return definition_;
}

} // namespace katana::runtime
