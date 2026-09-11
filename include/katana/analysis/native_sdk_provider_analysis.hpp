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
    SoundFrameService,
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

struct NativeStagedTransformLiteral final {
    std::uint32_t load_instruction_address = 0u;
    std::uint32_t literal_address = 0u;
    std::uint32_t value = 0u;
    std::string byte_identity;

    bool operator==(const NativeStagedTransformLiteral&) const = default;
};

// Positive source evidence for a wrapper which forwards r4 and stages into
// one fixed buffer, then restores the r5 spill for a second function when the
// first signed result is nonnegative. Its return instruction copies r14 to
// r0. A third, unconditional call follows the transform. These are syntactic
// register/spill paths: preserving the first result and the original r5 also
// requires all callees to preserve r14, r15 and the caller-owned spill slot.
//
// Neither callee is assumed to load files or decode PRS. Their effects and the
// finalizer's effects require independent provider contracts. In particular,
// this record grants no module entry, placement, completeness or ABI authority.
struct NativeStagedTransformCandidate final {
    std::uint32_t entry_address = 0u;
    std::uint32_t covered_size = 0u;
    std::string code_identity;
    std::uint32_t staging_buffer_address = 0u;
    std::uint32_t stage_call_address = 0u;
    std::uint32_t stage_target_address = 0u;
    std::uint32_t transform_call_address = 0u;
    std::uint32_t transform_target_address = 0u;
    std::uint32_t finalizer_call_address = 0u;
    std::uint32_t finalizer_target_address = 0u;
    // Out-of-body literals are part of the evidence identity as well. A
    // byte-identical wrapper with a different literal pool is a different
    // candidate, even if its own code_identity is unchanged.
    std::vector<NativeStagedTransformLiteral> literals;

    bool operator==(const NativeStagedTransformCandidate&) const = default;
};

[[nodiscard]] std::vector<NativeStagedTransformCandidate>
discover_native_staged_transform_candidates(
    const katana::io::ExecutableImage& image);

// Reuses an existing source-only CFG inventory. Every opcode is revalidated
// against the image, and all 24 wrapper instructions must be present together.
[[nodiscard]] std::vector<NativeStagedTransformCandidate>
discover_native_staged_transform_candidates(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> analyzed_lines);

struct NativePrsTransformLiteral final {
    std::uint32_t load_instruction_address = 0u;
    std::uint32_t literal_address = 0u;
    std::uint16_t value = 0u;
    std::string byte_identity;

    bool operator==(const NativePrsTransformLiteral&) const = default;
};

// A complete, call-free SH-4 implementation of the PRS token machine:
// LSB-first control bits, literals, 8/13-bit backward copies and a zero
// long-token terminator. This binds the entire control flow, register roles,
// saved-register restoration and both offset masks, rather than identifying
// a decoder from a filename, address, prologue or a few similar operations.
//
// Conditional effect: for a PRS stream accepted by the shared strict decoder,
// readable input and readable/writable output/scratch extents without wrap,
// it writes the same decoded bytes and returns their count in r0. Input,
// output, the eight bytes below incoming r15, and the bound code/literals
// must not overlap (including aliases). Incoming r15 must be 4-byte aligned
// and the output must fit the destination.
// The code/literals must still have these identities and no hook, concurrent
// writer or alternate entry may change this invocation's semantics. Without
// those independently established preconditions this is only source evidence.
// It does not prove a file read, residency, generation or module entry.
struct NativePrsTransformContract final {
    std::uint32_t entry_address = 0u;
    std::uint32_t covered_size = 0u;
    std::string code_identity;
    std::uint8_t source_register = 0u;
    std::uint8_t destination_register = 0u;
    std::uint16_t preserved_gpr_mask = 0u;
    std::vector<NativePrsTransformLiteral> literals;

    bool operator==(const NativePrsTransformContract&) const = default;
};

struct ControlFlowAnalysisResult;

// Possible PRS file/destination pair derived from calls in the current CFG.
// Conditional on SH-C callee-save registers, unchanged code/literals and the
// staged wrapper's caller-owned spill. This is NOT a successful read, loader
// effect, selector equivalence, actual placement or executable entry proof.
struct NativeConditionalFilePlacement final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t wrapper_address = 0u;
    std::string file_name;
    std::uint32_t possible_destination_address = 0u;
    std::uint32_t staging_buffer_address = 0u;
    // Binds the complete current source view, CFG derivation and assumptions.
    std::string source_contract_identity;
};

[[nodiscard]] std::vector<NativeConditionalFilePlacement>
discover_native_conditional_file_placements(
    const katana::io::ExecutableImage& image,
    const ControlFlowAnalysisResult& analysis);

// Reads and decodes the current image itself. An entry supplied by a caller
// is a probe location only; it is never promoted to an executable root.
[[nodiscard]] std::optional<NativePrsTransformContract>
recognize_native_prs_transform(const katana::io::ExecutableImage& image,
                               std::uint32_t entry_address);

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
