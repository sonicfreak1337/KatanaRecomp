#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t native_bringup_evidence_contract_version = 1u;
inline constexpr std::uint32_t native_bringup_artifact_format_version = 1u;
inline constexpr std::uint64_t native_bringup_artifact_maximum_size =
    4u * 1024u * 1024u;

// This is an authoring/admission state, never a runtime learning state.
// Observations may create work, but only independently re-proven immutable
// evidence can enter a precompiled bring-up allowlist.
enum class NativeBringupEvidenceStage : std::uint8_t {
    Observed,
    Candidate,
    Proven,
    RuntimeContract,
    Unresolved,
};

enum class NativeBringupTransferKind : std::uint8_t {
    CallRegister,
    TailJumpRegister,
};

enum class NativeBringupPromotionType : std::uint8_t {
    None,
    StaticCompiledTarget,
    ValidatedRuntimeContract,
    AnalyzerReproof,
};

struct NativeBringupTargetEvidence final {
    std::uint32_t contract_version =
        native_bringup_evidence_contract_version;
    NativeBringupEvidenceStage stage = NativeBringupEvidenceStage::Unresolved;
    NativeBringupTransferKind transfer_kind =
        NativeBringupTransferKind::CallRegister;

    std::uint32_t source_owner = 0u;
    std::uint32_t source_owner_size = 0u;
    // Exact compiled block which owns the terminal register transfer. The
    // callsite plus its architectural delay slot must end this block exactly;
    // an owner-wide range alone is not sufficient preflight authority.
    std::uint32_t source_block = 0u;
    std::uint32_t source_block_size = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t continuation = 0u;
    std::string_view source_owner_code_identity;
    std::string_view source_block_code_identity;
    // SHA-256 over the complete call instruction and its architectural delay
    // slot. For a register call this is exactly four bytes.
    std::string_view callsite_code_identity;

    std::uint32_t target = 0u;
    std::uint32_t target_block_size = 0u;
    std::uint32_t target_owner = 0u;
    std::uint32_t target_owner_size = 0u;
    std::string_view target_block_code_identity;
    std::string_view target_owner_code_identity;

    std::string_view source_image_id;
    std::string_view target_image_id;
    std::string_view source_module_identity;
    std::string_view target_module_identity;
    std::uint64_t source_generation = 0u;
    std::uint64_t target_generation = 0u;

    // Promotion-report seed. Runtime observations are deliberately kept
    // separate from immutable code identities and cannot mutate this record.
    std::string_view observation;
    std::string_view static_correlation;
    std::string_view missing_proof;
    NativeBringupPromotionType proposed_promotion =
        NativeBringupPromotionType::None;
    std::string_view analyzer_path;
    std::string_view runtime_contract_identity;
};

// Authoring state is not itself a runtime allowlist. Export derives the much
// smaller executable NativeBringup allowlist only after independently
// revalidating every execution-safety field against the current sealed AOT
// pack. Candidate may enter that non-release view while retaining an open
// proof task; only Proven is static proof and may contribute to StrictProduct.
struct NativeBringupAuthoringDefinition final {
    std::uint32_t contract_version =
        native_bringup_evidence_contract_version;
    std::string_view project_id;
    std::string_view project_version;
    // Exact committed analyzer product which was used to correlate the
    // evidence. It is data provenance, not permission to promote it.
    std::string_view analysis_identity;
    // Whole-pack semantic identity emitted by the strict exporter. This is
    // required in addition to per-target bytes: a runtime/adapter rebuild may
    // reuse the artifact only while the complete precompiled AOT universe is
    // byte-for-byte and ABI-for-ABI the same pack.
    std::string_view aot_pack_identity;
    // Monotone identity of the immutable AOT-pack authoring generation.
    // Runtime/adapter-only rebuilds keep this value unchanged.
    std::uint64_t aot_pack_generation = 0u;
    std::span<const NativeBringupTargetEvidence> targets;
};

void validate_native_bringup_authoring_definition(
    const NativeBringupAuthoringDefinition& definition);

[[nodiscard]] bool native_bringup_stage_is_static_proof(
    NativeBringupEvidenceStage stage) noexcept;

class NativeBringupAuthoringArtifact final {
  public:
    NativeBringupAuthoringArtifact(const NativeBringupAuthoringArtifact&) =
        delete;
    NativeBringupAuthoringArtifact& operator=(
        const NativeBringupAuthoringArtifact&) = delete;
    NativeBringupAuthoringArtifact(NativeBringupAuthoringArtifact&&) = delete;
    NativeBringupAuthoringArtifact& operator=(
        NativeBringupAuthoringArtifact&&) = delete;
    ~NativeBringupAuthoringArtifact() = default;

    [[nodiscard]] static std::shared_ptr<NativeBringupAuthoringArtifact>
    load(const std::filesystem::path& path);

    [[nodiscard]] static std::shared_ptr<NativeBringupAuthoringArtifact>
    write(const std::filesystem::path& path,
          const NativeBringupAuthoringDefinition& definition);

    [[nodiscard]] const std::filesystem::path& canonical_path() const noexcept;
    [[nodiscard]] const std::string& artifact_identity() const noexcept;
    [[nodiscard]] const NativeBringupAuthoringDefinition& definition() const
        noexcept;

  private:
    struct TargetStorage final {
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

    NativeBringupAuthoringArtifact() = default;
    void rebuild_definition();

    std::filesystem::path canonical_path_;
    std::string artifact_identity_;
    std::string project_id_;
    std::string project_version_;
    std::string analysis_identity_;
    std::string aot_pack_identity_;
    std::uint64_t aot_pack_generation_ = 0u;
    std::vector<TargetStorage> target_storage_;
    std::vector<NativeBringupTargetEvidence> targets_;
    NativeBringupAuthoringDefinition definition_;
};

} // namespace katana::runtime
