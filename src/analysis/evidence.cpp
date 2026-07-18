#include "katana/analysis/evidence.hpp"

namespace katana::analysis {

const char* control_flow_evidence_name(const ControlFlowEvidence evidence) noexcept {
    switch (evidence) {
    case ControlFlowEvidence::ProvenComplete:
        return "proven-complete";
    case ControlFlowEvidence::GuardedComplete:
        return "guarded-complete";
    case ControlFlowEvidence::GuardedPartial:
        return "guarded-partial";
    case ControlFlowEvidence::ForcedOverride:
        return "forced-override";
    case ControlFlowEvidence::HintCandidate:
        return "hint-candidate";
    case ControlFlowEvidence::RuntimeOnly:
        return "runtime-only";
    case ControlFlowEvidence::Unresolved:
        return "unresolved";
    }
    return "unresolved";
}

const char* analysis_evidence_origin_name(const AnalysisEvidenceOrigin origin) noexcept {
    switch (origin) {
    case AnalysisEvidenceOrigin::LocalValue:
        return "local-value";
    case AnalysisEvidenceOrigin::EntrySnapshot:
        return "entry-snapshot";
    case AnalysisEvidenceOrigin::FunctionSummary:
        return "function-summary";
    case AnalysisEvidenceOrigin::JumpTable:
        return "jump-table";
    case AnalysisEvidenceOrigin::UserOverride:
        return "user-override";
    case AnalysisEvidenceOrigin::UserHint:
        return "user-hint";
    case AnalysisEvidenceOrigin::RuntimeClassification:
        return "runtime-classification";
    }
    return "local-value";
}

} // namespace katana::analysis
