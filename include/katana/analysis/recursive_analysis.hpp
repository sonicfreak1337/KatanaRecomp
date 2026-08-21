#pragma once

#include "katana/analysis/evidence.hpp"
#include "katana/analysis/symbol_names.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::analysis {

enum class DiscoveredByteKind { Unknown, Code, Data };

struct ClassifiedRange {
    std::uint32_t start_address = 0u;
    std::uint64_t size = 0u;
    DiscoveredByteKind kind = DiscoveredByteKind::Unknown;
};

enum class FunctionOrigin {
    EntryPoint,
    DirectCall,
    IndirectCall,
    GuardedSnapshot,
    RuntimeCopy,
    JumpTableCall,
    UserOverride,
    UserHint,
    Symbol,
    StoredCodeAddress
};

enum class AnalysisConfidence { Low, Medium, High, Certain };

struct FunctionCandidate {
    std::uint32_t address = 0u;
    AnalysisConfidence confidence = AnalysisConfidence::Low;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::vector<FunctionOrigin> origins;
    // Non-zero only for an explicit exact function boundary. Inferred
    // candidates remain entry-only and therefore keep size zero.
    std::uint32_t size = 0u;
};

enum class AnalysisConflictKind { FunctionEntryInDelaySlot };

struct AnalysisConflict {
    std::uint32_t address = 0u;
    std::uint64_t size = 0u;
    AnalysisConflictKind kind = AnalysisConflictKind::FunctionEntryInDelaySlot;
};

enum class AnalysisDiagnosticKind {
    UnknownOpcode,
    ControlFlowInDelaySlot,
    DelaySlotUnavailable,
    DelaySlotUnknownOpcode
};

struct AnalysisDiagnostic {
    std::uint32_t address = 0u;
    std::uint16_t opcode = 0u;
    AnalysisDiagnosticKind kind = AnalysisDiagnosticKind::UnknownOpcode;
    std::string reason;
    ControlFlowEvidence evidence = ControlFlowEvidence::ProvenComplete;
};

[[nodiscard]] constexpr bool
analysis_diagnostic_blocks_codegen(const AnalysisDiagnostic& diagnostic) noexcept {
    switch (diagnostic.kind) {
    case AnalysisDiagnosticKind::UnknownOpcode:
    case AnalysisDiagnosticKind::DelaySlotUnknownOpcode:
        return control_flow_evidence_requires_static_decode(diagnostic.evidence);
    case AnalysisDiagnosticKind::ControlFlowInDelaySlot:
    case AnalysisDiagnosticKind::DelaySlotUnavailable:
        return true;
    }
    return true;
}

struct AnalysisSeed {
    std::uint32_t address = 0u;
    std::vector<FunctionOrigin> function_origins;
    bool guarded_candidate = false;
    ControlFlowEvidence evidence = ControlFlowEvidence::ProvenComplete;
    std::uint32_t function_size = 0u;
};

// Canonical semantic input retained only to validate incremental baselines.
// Origins and decode evidences are sorted unique sets; addresses are already
// resolved through the image address model.
struct RecursiveAnalysisSeedContract {
    std::uint32_t address = 0u;
    std::vector<FunctionOrigin> function_origins;
    std::vector<ControlFlowEvidence> decode_evidences;
    std::uint32_t function_size = 0u;

    bool operator==(const RecursiveAnalysisSeedContract&) const = default;
};

struct ContextualInstruction {
    katana::sh4::DisassemblyLine line;
    std::uint32_t incoming_address = 0u;
    std::optional<std::uint32_t> delay_slot_owner;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
};

namespace detail {
class RecursiveAnalysisSession;
}

struct RecursiveAnalysisResult;

enum class RecursiveAnalysisLimit : std::uint8_t {
    None,
    InstructionBudgetExceeded,
    ContextBudgetExceeded,
};

[[nodiscard]] constexpr std::string_view recursive_analysis_limit_name(
    const RecursiveAnalysisLimit limit) noexcept {
    switch (limit) {
    case RecursiveAnalysisLimit::None:
        return "none";
    case RecursiveAnalysisLimit::InstructionBudgetExceeded:
        return "instruction-budget-exceeded";
    case RecursiveAnalysisLimit::ContextBudgetExceeded:
        return "context-budget-exceeded";
    }
    return "unknown";
}

enum class RecursiveAnalysisBaselineStatus : std::uint8_t {
    NotRequested,
    Reused,
    ImageIdentityMismatch,
    ImageRevisionMismatch,
    IncompleteBaseline,
    BudgetIncompatible,
    SeedContractMismatch,
    PayloadMismatch,
    EvidenceUpgradeContextRetry,
};

[[nodiscard]] constexpr std::string_view
recursive_analysis_baseline_status_name(
    const RecursiveAnalysisBaselineStatus status) noexcept {
    switch (status) {
    case RecursiveAnalysisBaselineStatus::NotRequested:
        return "not-requested";
    case RecursiveAnalysisBaselineStatus::Reused:
        return "reused";
    case RecursiveAnalysisBaselineStatus::ImageIdentityMismatch:
        return "image-identity-mismatch";
    case RecursiveAnalysisBaselineStatus::ImageRevisionMismatch:
        return "image-revision-mismatch";
    case RecursiveAnalysisBaselineStatus::IncompleteBaseline:
        return "incomplete-baseline";
    case RecursiveAnalysisBaselineStatus::BudgetIncompatible:
        return "budget-incompatible";
    case RecursiveAnalysisBaselineStatus::SeedContractMismatch:
        return "seed-contract-mismatch";
    case RecursiveAnalysisBaselineStatus::PayloadMismatch:
        return "payload-mismatch";
    case RecursiveAnalysisBaselineStatus::EvidenceUpgradeContextRetry:
        return "evidence-upgrade-context-retry";
    }
    return "unknown";
}

struct RecursiveAnalysisOptions {
    // Complete monotone metadata/boundary contract. Exact ranges must always
    // be derived from this full set, including during incremental passes.
    std::vector<AnalysisSeed> additional_seeds;
    const RecursiveAnalysisResult* baseline = nullptr;
    // These are execution limits, not observation callbacks. The decoder
    // returns a typed partial result before admitting work beyond either cap.
    std::size_t maximum_instructions =
        std::numeric_limits<std::size_t>::max();
    std::size_t maximum_contexts =
        std::numeric_limits<std::size_t>::max();
};

// Physical work is part of the KR-4978 acceptance contract. These counters
// deliberately count semantic payload elements/bytes instead of allocator or
// wall-clock behaviour, so warm/cold regressions remain deterministic.
struct RecursiveAnalysisPhysicalWork {
    std::size_t trusted_snapshot_validations = 0u;
    std::size_t seed_arena_copy_items = 0u;
    std::size_t seed_arena_copy_bytes = 0u;
    std::size_t seed_arena_shift_items = 0u;
    std::size_t seed_arena_shift_bytes = 0u;
    std::size_t epoch_index_lookups = 0u;
    std::size_t epoch_index_updates = 0u;
    std::size_t terminal_epoch_fold_items = 0u;
    std::size_t seed_contract_items_visited = 0u;
    std::size_t decoded_work_items = 0u;
    std::size_t canonical_context_updates = 0u;
    std::size_t canonical_instruction_updates = 0u;
    std::size_t canonical_function_updates = 0u;
    std::size_t public_baseline_hash_bytes = 0u;
    std::size_t public_baseline_copy_items = 0u;
    std::size_t public_sort_items = 0u;
    std::size_t public_materialized_items = 0u;
    std::size_t public_materializations = 0u;

    void add(const RecursiveAnalysisPhysicalWork& other) noexcept {
        trusted_snapshot_validations += other.trusted_snapshot_validations;
        seed_arena_copy_items += other.seed_arena_copy_items;
        seed_arena_copy_bytes += other.seed_arena_copy_bytes;
        seed_arena_shift_items += other.seed_arena_shift_items;
        seed_arena_shift_bytes += other.seed_arena_shift_bytes;
        epoch_index_lookups += other.epoch_index_lookups;
        epoch_index_updates += other.epoch_index_updates;
        terminal_epoch_fold_items += other.terminal_epoch_fold_items;
        seed_contract_items_visited += other.seed_contract_items_visited;
        decoded_work_items += other.decoded_work_items;
        canonical_context_updates += other.canonical_context_updates;
        canonical_instruction_updates += other.canonical_instruction_updates;
        canonical_function_updates += other.canonical_function_updates;
        public_baseline_hash_bytes += other.public_baseline_hash_bytes;
        public_baseline_copy_items += other.public_baseline_copy_items;
        public_sort_items += other.public_sort_items;
        public_materialized_items += other.public_materialized_items;
        public_materializations += other.public_materializations;
    }
};

struct RecursiveAnalysisResult {
    static constexpr std::uint32_t baseline_contract_version = 1u;

    std::vector<katana::sh4::DisassemblyLine> instructions;
    std::vector<ContextualInstruction> contextual_instructions;
    std::vector<std::uint32_t> proven_instruction_addresses;
    std::vector<std::uint32_t> guarded_candidate_instruction_addresses;
    std::vector<ClassifiedRange> ranges;
    std::vector<ClassifiedRange> unreachable_code;
    std::vector<FunctionCandidate> functions;
    std::vector<AnalysisConflict> conflicts;
    std::vector<AnalysisDiagnostic> diagnostics;
    std::size_t processed_work_items = 0u;
    std::size_t reused_contexts = 0u;
    RecursiveAnalysisLimit limit = RecursiveAnalysisLimit::None;
    // O(1) image binding plus the canonical seed contract make the public
    // baseline pointer fail-safe. A foreign, mutated, partial, or
    // non-monotone baseline is ignored and recomputed from the full input.
    std::uint32_t retained_baseline_contract_version = 0u;
    std::uint64_t source_image_identity = 0u;
    std::uint64_t source_image_revision = 0u;
    std::vector<RecursiveAnalysisSeedContract> seed_contract;
    RecursiveAnalysisBaselineStatus baseline_status =
        RecursiveAnalysisBaselineStatus::NotRequested;
    RecursiveAnalysisPhysicalWork physical_work;

  private:
    // Seal over every payload field consumed by incremental reuse. Public
    // result mutation remains useful to callers, but can never turn modified
    // vectors into an accepted analyzer baseline.
    std::string retained_baseline_payload_sha256_;

    friend RecursiveAnalysisResult analyze_reachable_code(
        const katana::io::ExecutableImage& image,
        const RecursiveAnalysisOptions& options);
    friend class detail::RecursiveAnalysisSession;
};

namespace detail {

// The journal is authoritative only inside one CFA-owned session. Public
// callers never provide it and therefore continue through the full canonical
// baseline validation path above.
struct RecursiveAnalysisDeltaJournal {
    std::span<const AnalysisSeed> changed_seeds;
    bool exact_function_boundary_changed = false;
    bool complete = true;
    // Distinguishes an intentionally empty complete seed contract from the
    // empty complete_seeds span used by a trusted delta round. If the
    // session discovers that a presumed delta round must go cold, it returns
    // a typed retry without publishing an incomplete epoch.
    bool complete_seed_contract_supplied = false;
};

class RecursiveAnalysisSnapshot final {
  public:
    RecursiveAnalysisSnapshot() = default;

    [[nodiscard]] std::uint64_t epoch_version() const noexcept {
        return epoch_version_;
    }
    [[nodiscard]] std::size_t instruction_count() const noexcept {
        return instruction_count_;
    }
    [[nodiscard]] std::size_t function_count() const noexcept {
        return function_count_;
    }
    [[nodiscard]] std::size_t contextual_instruction_count() const noexcept {
        return contextual_instruction_count_;
    }
    [[nodiscard]] RecursiveAnalysisLimit limit() const noexcept {
        return limit_;
    }
    [[nodiscard]] RecursiveAnalysisBaselineStatus baseline_status() const noexcept {
        return baseline_status_;
    }
    [[nodiscard]] std::size_t processed_work_items() const noexcept {
        return processed_work_items_;
    }
    [[nodiscard]] std::size_t reused_contexts() const noexcept {
        return reused_contexts_;
    }
    [[nodiscard]] std::span<const std::uint32_t>
    changed_instruction_addresses() const noexcept {
        return changed_instruction_addresses_;
    }
    [[nodiscard]] std::span<const katana::sh4::DisassemblyLine>
    changed_instructions() const noexcept {
        return changed_instructions_;
    }
    [[nodiscard]] std::span<const std::uint32_t>
    changed_function_entries() const noexcept {
        return changed_function_entries_;
    }
    [[nodiscard]] std::span<const FunctionCandidate>
    changed_functions() const noexcept {
        return changed_functions_;
    }
    [[nodiscard]] std::span<const std::uint32_t>
    newly_proven_instruction_addresses() const noexcept {
        return newly_proven_instruction_addresses_;
    }
    [[nodiscard]] const RecursiveAnalysisPhysicalWork&
    physical_work() const noexcept {
        return physical_work_;
    }
    [[nodiscard]] bool cold_retry_required() const noexcept {
        return cold_retry_required_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return owner_identity_ != nullptr && epoch_data_ != nullptr &&
               !cold_retry_required_;
    }

  private:
    // A shared identity token prevents raw-Impl-pointer ABA after a Session
    // is destroyed while one of its immutable snapshots remains alive.
    std::shared_ptr<const void> owner_identity_;
    const void* epoch_data_ = nullptr;
    std::shared_ptr<const void> epoch_keepalive_;
    std::uint64_t epoch_version_ = 0u;
    std::size_t instruction_count_ = 0u;
    std::size_t function_count_ = 0u;
    std::size_t contextual_instruction_count_ = 0u;
    RecursiveAnalysisLimit limit_ = RecursiveAnalysisLimit::None;
    RecursiveAnalysisBaselineStatus baseline_status_ =
        RecursiveAnalysisBaselineStatus::NotRequested;
    std::size_t processed_work_items_ = 0u;
    std::size_t reused_contexts_ = 0u;
    std::span<const std::uint32_t> changed_instruction_addresses_;
    std::span<const katana::sh4::DisassemblyLine> changed_instructions_;
    std::span<const std::uint32_t> changed_function_entries_;
    std::span<const FunctionCandidate> changed_functions_;
    std::span<const std::uint32_t> newly_proven_instruction_addresses_;
    RecursiveAnalysisPhysicalWork physical_work_;
    bool cold_retry_required_ = false;

    friend class RecursiveAnalysisSession;
};

class RecursiveAnalysisSession final {
  public:
    RecursiveAnalysisSession();
    ~RecursiveAnalysisSession();
    RecursiveAnalysisSession(RecursiveAnalysisSession&&) noexcept;
    RecursiveAnalysisSession& operator=(RecursiveAnalysisSession&&) noexcept;
    RecursiveAnalysisSession(const RecursiveAnalysisSession&) = delete;
    RecursiveAnalysisSession& operator=(const RecursiveAnalysisSession&) = delete;

    // Same-image root additions may extend the retained immutable decode
    // epoch. Root removal, immutable proof/content/ABI mutation, an incomplete
    // producer journal or a changed exact boundary rejects the baseline
    // fail-closed. Absence from the supplied disassembly is never treated as
    // negative reachability evidence.
    [[nodiscard]] RecursiveAnalysisSnapshot analyze(
        const katana::io::ExecutableImage& image,
        std::span<const AnalysisSeed> complete_seeds,
        const RecursiveAnalysisDeltaJournal& delta,
        std::size_t maximum_instructions =
            std::numeric_limits<std::size_t>::max(),
        std::size_t maximum_contexts =
            std::numeric_limits<std::size_t>::max());

    // Public result vectors, classifications and the mutation-detection seal
    // are constructed exactly here. CFA calls this once at its terminal
    // relational state or typed early exit.
    [[nodiscard]] RecursiveAnalysisResult materialize(
        const katana::io::ExecutableImage& image,
        const RecursiveAnalysisSnapshot& snapshot);

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace detail

[[nodiscard]] RecursiveAnalysisResult
analyze_reachable_code(const katana::io::ExecutableImage& image,
                       const RecursiveAnalysisOptions& options = {});

[[nodiscard]] const char* discovered_byte_kind_name(DiscoveredByteKind kind) noexcept;
[[nodiscard]] const char* function_origin_name(FunctionOrigin origin) noexcept;
[[nodiscard]] const char* analysis_confidence_name(AnalysisConfidence confidence) noexcept;
[[nodiscard]] const char* analysis_conflict_kind_name(AnalysisConflictKind kind) noexcept;
[[nodiscard]] std::string
format_recursive_analysis_report(const RecursiveAnalysisResult& result,
                                 std::span<const SymbolicAddress> symbols = {});

} // namespace katana::analysis
