#include "katana/codegen/prepared_native_port_admission_artifact.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::codegen {
namespace {

constexpr std::string_view artifact_magic{
    "katana-prepared-native-port-admission-v1"};
constexpr std::size_t maximum_string_bytes = 64u * 1024u;
constexpr std::size_t maximum_collection_items = 4u * 1024u * 1024u;
constexpr std::size_t maximum_allocation_bytes =
    2u * maximum_prepared_native_port_admission_artifact_bytes;

class CodecError final : public std::runtime_error {
  public:
    CodecError()
        : std::runtime_error(
              "prepared-native-port-admission-artifact-codec") {}
};

class Writer final {
  public:
    explicit Writer(const std::size_t maximum) : maximum_(maximum) {}

    void u8(const std::uint8_t value) { append(&value, sizeof(value)); }
    void u32(const std::uint32_t value) {
        std::array<std::uint8_t, 4u> bytes{};
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] =
                static_cast<std::uint8_t>(value >> (index * 8u));
        append(bytes.data(), bytes.size());
    }
    void u64(const std::uint64_t value) {
        std::array<std::uint8_t, 8u> bytes{};
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] =
                static_cast<std::uint8_t>(value >> (index * 8u));
        append(bytes.data(), bytes.size());
    }
    void text(const std::string_view value) {
        if (value.size() > maximum_string_bytes ||
            value.size() > std::numeric_limits<std::uint32_t>::max())
            throw CodecError();
        u32(static_cast<std::uint32_t>(value.size()));
        append(value.data(), value.size());
    }
    void blob(const std::span<const std::uint8_t> value) {
        u64(value.size());
        append(value.data(), value.size());
    }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return bytes_;
    }
    void patch_u64(const std::size_t offset, const std::uint64_t value) {
        if (offset > bytes_.size() ||
            sizeof(value) > bytes_.size() - offset)
            throw CodecError();
        for (std::size_t index = 0u; index < sizeof(value); ++index)
            bytes_[offset + index] =
                static_cast<std::uint8_t>(value >> (index * 8u));
    }
    void patch_text_bytes(const std::size_t offset,
                          const std::string_view value) {
        if (offset > bytes_.size() || value.size() > bytes_.size() - offset)
            throw CodecError();
        std::copy(value.begin(), value.end(), bytes_.begin() + offset);
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    void append(const void* const data, const std::size_t size) {
        if (size == 0u) return;
        if (size > maximum_ - bytes_.size()) throw CodecError();
        const auto* first = static_cast<const std::uint8_t*>(data);
        bytes_.insert(bytes_.end(), first, first + size);
    }

    std::size_t maximum_ = 0u;
    std::vector<std::uint8_t> bytes_;
};

class Reader final {
  public:
    explicit Reader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() { return take(1u)[0u]; }
    [[nodiscard]] std::uint32_t u32() {
        const auto bytes = take(4u);
        std::uint32_t value = 0u;
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            value |= static_cast<std::uint32_t>(bytes[index]) <<
                     (index * 8u);
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        const auto bytes = take(8u);
        std::uint64_t value = 0u;
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            value |= static_cast<std::uint64_t>(bytes[index]) <<
                     (index * 8u);
        return value;
    }
    [[nodiscard]] std::string text() {
        const auto size = u32();
        if (size > maximum_string_bytes) throw CodecError();
        charge(size);
        const auto bytes = take(size);
        return std::string(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    [[nodiscard]] std::span<const std::uint8_t> blob() {
        const auto size = u64();
        if (size > std::numeric_limits<std::size_t>::max())
            throw CodecError();
        const auto retained_size = static_cast<std::size_t>(size);
        charge(retained_size);
        return take(retained_size);
    }
    template <typename T>
    [[nodiscard]] std::size_t count(
        const std::size_t maximum = maximum_collection_items) {
        const auto value = u64();
        if (value > maximum || value > std::numeric_limits<std::size_t>::max())
            throw CodecError();
        const auto result = static_cast<std::size_t>(value);
        if (result > maximum_allocation_bytes / sizeof(T))
            throw CodecError();
        charge(result * sizeof(T));
        return result;
    }
    [[nodiscard]] bool empty() const noexcept {
        return cursor_ == bytes_.size();
    }

  private:
    [[nodiscard]] std::span<const std::uint8_t> take(
        const std::size_t size) {
        if (cursor_ > bytes_.size() || size > bytes_.size() - cursor_)
            throw CodecError();
        const auto result = bytes_.subspan(cursor_, size);
        cursor_ += size;
        return result;
    }
    void charge(const std::size_t bytes) {
        if (bytes > maximum_allocation_bytes - allocation_bytes_)
            throw CodecError();
        allocation_bytes_ += bytes;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
    std::size_t allocation_bytes_ = 0u;
};

[[nodiscard]] bool valid_sha256(const std::string_view value) noexcept {
    return value.size() == 64u &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool valid_prefixed_sha256(
    const std::string_view value) noexcept {
    constexpr std::string_view prefix{"sha256:"};
    return value.starts_with(prefix) &&
           valid_sha256(value.substr(prefix.size()));
}

void append_key_field(std::string& material, const std::string_view value) {
    material.push_back('s');
    material += std::to_string(value.size());
    material.push_back(':');
    material.append(value);
    material.push_back(';');
}

void append_key_value(std::string& material, const std::uint64_t value) {
    material.push_back('i');
    material += std::to_string(value);
    material.push_back(';');
}

void write_identity(
    Writer& output,
    const PreparedNativePortAdmissionArtifactIdentity& identity) {
    output.text(identity.key);
    output.text(identity.analysis_artifact_identity);
    output.text(identity.analysis_archive_sha256);
    output.text(identity.game_project_identity);
    output.text(identity.native_port_identity);
    output.text(identity.native_port_artifact_identity);
    output.text(identity.admission_implementation_identity);
    output.u32(identity.analyzer_abi);
    output.u32(identity.backend_abi);
}

PreparedNativePortAdmissionArtifactIdentity read_identity(Reader& input) {
    PreparedNativePortAdmissionArtifactIdentity identity;
    identity.key = input.text();
    identity.analysis_artifact_identity = input.text();
    identity.analysis_archive_sha256 = input.text();
    identity.game_project_identity = input.text();
    identity.native_port_identity = input.text();
    identity.native_port_artifact_identity = input.text();
    identity.admission_implementation_identity = input.text();
    identity.analyzer_abi = input.u32();
    identity.backend_abi = input.u32();
    return identity;
}

void write_function_digests(
    Writer& output,
    const std::vector<PreparedNativePortAdmissionFunctionDigest>& values) {
    output.u64(values.size());
    for (const auto& function : values) {
        output.u32(function.function_entry);
        output.text(function.digest);
        output.u64(function.blocks.size());
        for (const auto& block : function.blocks) {
            output.u32(block.block_address);
            output.text(block.digest);
        }
    }
}

std::vector<PreparedNativePortAdmissionFunctionDigest>
read_function_digests(Reader& input) {
    const auto count =
        input.count<PreparedNativePortAdmissionFunctionDigest>();
    std::vector<PreparedNativePortAdmissionFunctionDigest> result;
    result.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        PreparedNativePortAdmissionFunctionDigest function;
        function.function_entry = input.u32();
        function.digest = input.text();
        const auto block_count =
            input.count<PreparedNativePortAdmissionBlockDigest>();
        function.blocks.reserve(block_count);
        for (std::size_t block_index = 0u; block_index < block_count;
             ++block_index) {
            function.blocks.push_back(
                {input.u32(), input.text()});
        }
        result.push_back(std::move(function));
    }
    return result;
}

[[nodiscard]] bool canonical_function_digests(
    const std::vector<PreparedNativePortAdmissionFunctionDigest>& values)
    noexcept {
    std::uint32_t prior_function = 0u;
    bool first_function = true;
    for (const auto& function : values) {
        if (!valid_sha256(function.digest) || function.blocks.empty() ||
            (!first_function && function.function_entry <= prior_function))
            return false;
        first_function = false;
        prior_function = function.function_entry;
        std::uint32_t prior_block = 0u;
        bool first_block = true;
        for (const auto& block : function.blocks) {
            if (!valid_sha256(block.digest) ||
                (!first_block && block.block_address <= prior_block))
                return false;
            first_block = false;
            prior_block = block.block_address;
        }
    }
    return !values.empty();
}

} // namespace

std::string prepared_native_port_admission_artifact_identity_key(
    const PreparedNativePortAdmissionArtifactIdentity& identity) {
    std::string material;
    material.reserve(1024u);
    append_key_field(
        material, "katana-prepared-native-port-admission-identity-v1");
    append_key_value(
        material, prepared_native_port_admission_artifact_schema_version);
    append_key_value(
        material, prepared_native_port_admission_artifact_codec_version);
    append_key_field(material, identity.analysis_artifact_identity);
    append_key_field(material, identity.analysis_archive_sha256);
    append_key_field(material, identity.game_project_identity);
    append_key_field(material, identity.native_port_identity);
    append_key_field(material, identity.native_port_artifact_identity);
    append_key_field(material, identity.admission_implementation_identity);
    append_key_value(material, identity.analyzer_abi);
    append_key_value(material, identity.backend_abi);
    return katana::io::sha256_bytes(material);
}

bool prepared_native_port_admission_artifact_cacheable(
    const PreparedNativePortAdmissionArtifact& artifact) noexcept {
    try {
        const auto& identity = artifact.identity;
        return valid_sha256(identity.key) &&
               identity.key ==
                   prepared_native_port_admission_artifact_identity_key(
                       identity) &&
               valid_sha256(identity.analysis_artifact_identity) &&
               valid_sha256(identity.analysis_archive_sha256) &&
               valid_prefixed_sha256(identity.game_project_identity) &&
               valid_sha256(identity.native_port_identity) &&
               !identity.native_port_artifact_identity.empty() &&
               valid_sha256(
                   identity.admission_implementation_identity) &&
               identity.analyzer_abi != 0u && identity.backend_abi != 0u &&
               valid_sha256(artifact.emitted_program_digest) &&
               canonical_function_digests(artifact.function_digests) &&
               !artifact.admission_payload.empty() &&
               artifact.admission_payload.size() <=
                   maximum_prepared_native_port_admission_artifact_bytes;
    } catch (...) {
        return false;
    }
}

std::vector<std::uint8_t>
serialize_prepared_native_port_admission_artifact(
    const PreparedNativePortAdmissionArtifact& artifact) {
    if (!prepared_native_port_admission_artifact_cacheable(artifact))
        throw std::invalid_argument(
            "Prepared-NativePort-Admission-Artefakt ist nicht cachebar.");

    Writer output(maximum_prepared_native_port_admission_artifact_bytes);
    output.text(artifact_magic);
    output.u32(prepared_native_port_admission_artifact_schema_version);
    output.u32(prepared_native_port_admission_artifact_codec_version);
    output.text(artifact.identity.key);
    constexpr std::size_t sha256_text_bytes = 64u;
    const auto payload_sha_offset = output.size() + sizeof(std::uint32_t);
    output.text(std::string(sha256_text_bytes, '0'));
    const auto payload_size_offset = output.size();
    output.u64(0u);
    const auto payload_offset = output.size();

    write_identity(output, artifact.identity);
    output.text(artifact.emitted_program_digest);
    write_function_digests(output, artifact.function_digests);
    output.blob(artifact.admission_payload);

    const auto payload_size = output.size() - payload_offset;
    const auto payload = output.bytes().subspan(payload_offset, payload_size);
    const auto payload_sha = katana::io::sha256_bytes(std::string_view(
        reinterpret_cast<const char*>(payload.data()), payload.size()));
    if (payload_sha.size() != sha256_text_bytes) throw CodecError();
    output.patch_u64(payload_size_offset, payload_size);
    output.patch_text_bytes(payload_sha_offset, payload_sha);
    return std::move(output).finish();
}

PreparedNativePortAdmissionArtifactParseResult
parse_prepared_native_port_admission_artifact(
    const std::string_view expected_key,
    const std::span<const std::uint8_t> bytes) {
    if (expected_key.empty())
        return {PreparedNativePortAdmissionArtifactState::Miss,
                {},
                "expected-key-empty"};
    if (bytes.empty() ||
        bytes.size() >
            maximum_prepared_native_port_admission_artifact_bytes)
        return {PreparedNativePortAdmissionArtifactState::Corrupt,
                {},
                "artifact-size"};
    std::string_view decode_stage = "envelope";
    try {
        Reader envelope(bytes);
        if (envelope.text() != artifact_magic ||
            envelope.u32() !=
                prepared_native_port_admission_artifact_schema_version ||
            envelope.u32() !=
                prepared_native_port_admission_artifact_codec_version)
            return {PreparedNativePortAdmissionArtifactState::Miss,
                    {},
                    "schema"};
        const auto stored_key = envelope.text();
        if (stored_key != expected_key)
            return {PreparedNativePortAdmissionArtifactState::Miss,
                    {},
                    "identity-key"};
        const auto expected_sha = envelope.text();
        const auto payload_bytes = envelope.blob();
        if (!envelope.empty()) throw CodecError();
        const auto actual_sha = katana::io::sha256_bytes(std::string_view(
            reinterpret_cast<const char*>(payload_bytes.data()),
            payload_bytes.size()));
        if (expected_sha != actual_sha) throw CodecError();

        Reader payload(payload_bytes);
        PreparedNativePortAdmissionArtifact artifact;
        decode_stage = "identity";
        artifact.identity = read_identity(payload);
        if (artifact.identity.key != expected_key ||
            artifact.identity.key !=
                prepared_native_port_admission_artifact_identity_key(
                    artifact.identity))
            throw CodecError();
        decode_stage = "program-digests";
        artifact.emitted_program_digest = payload.text();
        artifact.function_digests = read_function_digests(payload);
        decode_stage = "admission-payload";
        const auto encoded_payload = payload.blob();
        artifact.admission_payload.assign(
            encoded_payload.begin(), encoded_payload.end());
        decode_stage = "artifact-contract";
        if (!payload.empty() ||
            !prepared_native_port_admission_artifact_cacheable(artifact))
            throw CodecError();
        return {PreparedNativePortAdmissionArtifactState::Hit,
                std::move(artifact),
                "hit"};
    } catch (const std::bad_alloc&) {
        return {PreparedNativePortAdmissionArtifactState::Corrupt,
                {},
                "allocation"};
    } catch (const std::exception&) {
        return {PreparedNativePortAdmissionArtifactState::Corrupt,
                {},
                "codec-" + std::string(decode_stage)};
    }
}

} // namespace katana::codegen
