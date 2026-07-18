#pragma once

#include "katana/analysis/analysis_index.hpp"
#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/symbol_names.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace katana::analysis {

enum class AnalysisDirectiveDiagnosticStatus : std::uint8_t {
    Accepted,
    Confirmed,
    Rejected,
    Stale
};

struct AnalysisDirectiveDiagnostic {
    std::size_t line = 0u;
    std::uint32_t address = 0u;
    AnalysisDirectiveDiagnosticStatus status = AnalysisDirectiveDiagnosticStatus::Accepted;
    std::string reason;
};

struct ControlFlowSite {
    std::uint32_t instruction_address = 0u;
    IndirectControlFlowKind kind = IndirectControlFlowKind::Jump;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    IndirectControlFlowOriginClass origin_class = IndirectControlFlowOriginClass::NotApplicable;
    std::vector<AnalysisEvidenceOrigin> evidence_origins;
    std::vector<std::uint32_t> targets;
    std::vector<std::uint32_t> evidence_call_sites;
    std::vector<std::uint32_t> evidence_callees;
};

struct ControlFlowAnalysisResult {
    RecursiveAnalysisResult recursive;
    std::vector<IndirectControlFlowResolution> indirect_control_flow;
    std::vector<JumpTableAnalysis> jump_tables;
    std::vector<FunctionValueSummary> function_value_summaries;
    std::vector<ResolvedControlFlowEdge> resolved_edges;
    std::vector<ControlFlowSite> sites;
    std::shared_ptr<const InstructionArena> instruction_arena;
    std::vector<InstructionSpan> block_spans;
    EvidenceInterner evidence_ids;
    JumpTableCacheCounters jump_table_cache;
    std::size_t fixpoint_iterations = 0u;
    std::size_t function_summary_iterations = 0u;
    std::size_t function_scc_count = 0u;
    std::size_t unchanged_ingress_skips = 0u;
    std::vector<AnalysisDirectiveDiagnostic> directive_diagnostics;
    std::vector<SymbolicAddress> symbolic_addresses;
};

[[nodiscard]] const char*
analysis_directive_diagnostic_status_name(AnalysisDirectiveDiagnosticStatus status) noexcept;

[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(const katana::io::ExecutableImage& image,
                     const AnalysisOverrides* overrides = nullptr);

} // namespace katana::analysis
