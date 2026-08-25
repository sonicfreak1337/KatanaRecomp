#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::codegen {

inline constexpr std::uint32_t
    prepared_native_port_admission_artifact_schema_version = 1u;
inline constexpr std::uint32_t
    prepared_native_port_admission_artifact_codec_version = 1u;
inline constexpr std::size_t
    maximum_prepared_native_port_admission_artifact_bytes =
        256u * 1024u * 1024u;

// Exact, path-free dependencies of a prepared NativePort admission.  The
// analysis archive digest deliberately accompanies its logical identity: a
// committed generation may reuse admission only for the exact bytes which
// were source- and ledger-validated by the caller.
struct PreparedNativePortAdmissionArtifactIdentity final {
    std::string key;
    std::string analysis_artifact_identity;
    std::string analysis_archive_sha256;
    std::string game_project_identity;
    std::string native_port_identity;
    std::string native_port_artifact_identity;
    std::string admission_implementation_identity;
    std::uint32_t analyzer_abi = 0u;
    std::uint32_t backend_abi = 0u;

    [[nodiscard]] bool operator==(
        const PreparedNativePortAdmissionArtifactIdentity&) const = default;
};

struct PreparedNativePortAdmissionBlockDigest final {
    std::uint32_t block_address = 0u;
    std::string digest;

    [[nodiscard]] bool operator==(
        const PreparedNativePortAdmissionBlockDigest&) const = default;
};

struct PreparedNativePortAdmissionFunctionDigest final {
    std::uint32_t function_entry = 0u;
    std::string digest;
    std::vector<PreparedNativePortAdmissionBlockDigest> blocks;

    [[nodiscard]] bool operator==(
        const PreparedNativePortAdmissionFunctionDigest&) const = default;
};

// The typed dependency ledger remains visible to the caller.  admission_payload
// is an opaque, versioned encoding owned by the admission implementation; it
// contains the pointer-free final ProgramIndex, closure and emission-facing
// descriptors.  Keeping the envelope independent lets hostile/corrupt cache
// bytes be rejected before any internal state is rehydrated.
struct PreparedNativePortAdmissionArtifact final {
    PreparedNativePortAdmissionArtifactIdentity identity;
    std::string emitted_program_digest;
    std::vector<PreparedNativePortAdmissionFunctionDigest> function_digests;
    std::vector<std::uint8_t> admission_payload;

    [[nodiscard]] bool operator==(
        const PreparedNativePortAdmissionArtifact&) const = default;
};

enum class PreparedNativePortAdmissionArtifactState : std::uint8_t {
    Miss = 0u,
    Hit,
    Corrupt,
};

struct PreparedNativePortAdmissionArtifactParseResult final {
    PreparedNativePortAdmissionArtifactState state =
        PreparedNativePortAdmissionArtifactState::Miss;
    PreparedNativePortAdmissionArtifact artifact;
    std::string reason;
};

[[nodiscard]] std::string prepared_native_port_admission_artifact_identity_key(
    const PreparedNativePortAdmissionArtifactIdentity& identity);

[[nodiscard]] bool prepared_native_port_admission_artifact_cacheable(
    const PreparedNativePortAdmissionArtifact& artifact) noexcept;

[[nodiscard]] std::vector<std::uint8_t>
serialize_prepared_native_port_admission_artifact(
    const PreparedNativePortAdmissionArtifact& artifact);

[[nodiscard]] PreparedNativePortAdmissionArtifactParseResult
parse_prepared_native_port_admission_artifact(
    std::string_view expected_key,
    std::span<const std::uint8_t> bytes);

} // namespace katana::codegen
