#pragma once

#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::analysis {

// Semantic SDK boundaries are discovered from bounded SH-4 dataflow shapes,
// never from title addresses or symbol names.  A candidate remains only
// export-time evidence until a title supplies a native provider symbol and
// the ordinary NativePort hook proof binds its exact range and byte identity.
enum class NativeSdkProviderFamily : std::uint8_t {
    NamedTextureArchiveLoad,
    TextureArchiveRelease,
    SynchronousContentRangeRead,
    SoundBankChunkRegistration,
};

enum class NativeSdkProviderBoundaryProof : std::uint8_t {
    StructuralPrologueReturn,
};

enum class NativeSdkResourceReferenceKind : std::uint8_t {
    GuestDescriptorPointer,
};

// A provider-owned record field may be either an opaque scalar or a guest
// pointer that surviving AOT code continues to dereference.  The analyzer
// only emits this contract after it has proved both the owner-record stride
// and concrete downstream descriptor accesses.  Runtime adapters can then
// materialize the required guest structure instead of leaking a host token
// into original SH-4 code.
struct NativeSdkResourceReferenceContract final {
    NativeSdkResourceReferenceKind kind =
        NativeSdkResourceReferenceKind::GuestDescriptorPointer;
    std::uint32_t owner_record_stride = 0u;
    std::uint32_t reference_field_offset = 0u;
    std::uint32_t descriptor_stride = 0u;
    std::uint32_t minimum_descriptor_bytes = 0u;
    std::vector<std::uint32_t> observed_read_offsets;
    std::vector<std::uint32_t> observed_write_offsets;
    std::vector<std::uint32_t> evidence_sites;

    bool operator==(const NativeSdkResourceReferenceContract&) const =
        default;
};

struct NativeSdkProviderCandidate final {
    NativeSdkProviderFamily family =
        NativeSdkProviderFamily::NamedTextureArchiveLoad;
    std::uint32_t entry_address = 0u;
    std::uint32_t covered_size = 0u;
    std::string code_identity;
    NativeSdkProviderBoundaryProof boundary_proof =
        NativeSdkProviderBoundaryProof::StructuralPrologueReturn;
    std::optional<NativeSdkResourceReferenceContract> resource_reference;
    std::vector<std::string> evidence;

    bool operator==(const NativeSdkProviderCandidate&) const = default;
};

[[nodiscard]] std::string_view native_sdk_provider_family_name(
    NativeSdkProviderFamily family) noexcept;
[[nodiscard]] std::string_view native_sdk_provider_boundary_proof_name(
    NativeSdkProviderBoundaryProof proof) noexcept;
[[nodiscard]] std::string_view native_sdk_resource_reference_kind_name(
    NativeSdkResourceReferenceKind kind) noexcept;

// The image overload scans executable committed bytes directly and is suited
// to private manifest authoring before a whole-program analysis exists.  The
// line overload reuses the final analyzer inventory during product export.
// If consumers of a discovered resource descriptor live outside that root
// slice, only the missing data contract is completed from the same bounded,
// identity-bound executable image; provider roots are never added that way.
// Both paths apply the same exact structural recognizer and produce the same
// sorted, identity-bound candidates.
[[nodiscard]] std::vector<NativeSdkProviderCandidate>
discover_native_sdk_provider_candidates(
    const katana::io::ExecutableImage& image);

[[nodiscard]] std::vector<NativeSdkProviderCandidate>
discover_native_sdk_provider_candidates(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> analyzed_lines);

} // namespace katana::analysis
