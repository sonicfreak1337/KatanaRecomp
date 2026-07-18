#pragma once

#include <cstdint>

namespace katana::analysis {

enum class ControlFlowEvidence : std::uint8_t {
    ProvenComplete,
    GuardedComplete,
    GuardedPartial,
    ForcedOverride,
    HintCandidate,
    RuntimeOnly,
    Unresolved
};

enum class AnalysisEvidenceOrigin : std::uint8_t {
    LocalValue,
    EntrySnapshot,
    FunctionSummary,
    JumpTable,
    UserOverride,
    UserHint,
    RuntimeClassification
};

[[nodiscard]] constexpr bool
control_flow_evidence_complete(const ControlFlowEvidence evidence) noexcept {
    return evidence == ControlFlowEvidence::ProvenComplete ||
           evidence == ControlFlowEvidence::GuardedComplete;
}

[[nodiscard]] constexpr bool
control_flow_evidence_proven(const ControlFlowEvidence evidence) noexcept {
    return evidence == ControlFlowEvidence::ProvenComplete;
}

[[nodiscard]] constexpr bool
control_flow_evidence_runtime_default(const ControlFlowEvidence evidence) noexcept {
    return !control_flow_evidence_complete(evidence);
}

[[nodiscard]] constexpr std::uint8_t
control_flow_evidence_strength(const ControlFlowEvidence evidence) noexcept {
    switch (evidence) {
    case ControlFlowEvidence::ProvenComplete:
        return 6u;
    case ControlFlowEvidence::GuardedComplete:
        return 5u;
    case ControlFlowEvidence::GuardedPartial:
        return 4u;
    case ControlFlowEvidence::ForcedOverride:
        return 3u;
    case ControlFlowEvidence::HintCandidate:
        return 2u;
    case ControlFlowEvidence::RuntimeOnly:
        return 1u;
    case ControlFlowEvidence::Unresolved:
        return 0u;
    }
    return 0u;
}

[[nodiscard]] const char* control_flow_evidence_name(ControlFlowEvidence evidence) noexcept;
[[nodiscard]] const char* analysis_evidence_origin_name(AnalysisEvidenceOrigin origin) noexcept;

} // namespace katana::analysis
