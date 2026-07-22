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
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
    std::vector<StaticReturnContinuationCandidate> static_return_continuations;
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
    std::size_t function_iteration_budget = 0u;
    bool function_budget_exhausted = false;
    std::vector<AnalysisDirectiveDiagnostic> directive_diagnostics;
    std::vector<SymbolicAddress> symbolic_addresses;
};

struct ControlFlowAnalysisProgress {
    std::string_view phase;
    std::size_t iteration = 0u;
    std::size_t seeds = 0u;
    std::size_t instructions = 0u;
    std::size_t contexts = 0u;
    std::size_t resolutions = 0u;
};

using ControlFlowAnalysisProgressCallback =
    std::function<void(const ControlFlowAnalysisProgress& progress)>;

[[nodiscard]] const char*
analysis_directive_diagnostic_status_name(AnalysisDirectiveDiagnosticStatus status) noexcept;

[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(const katana::io::ExecutableImage& image,
                     const AnalysisOverrides* overrides = nullptr);

[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(const katana::io::ExecutableImage& image,
                     const AnalysisOverrides* overrides,
                     const ControlFlowAnalysisProgressCallback& progress_callback);

} // namespace katana::analysis
