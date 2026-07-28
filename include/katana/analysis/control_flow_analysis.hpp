#pragma once

#include "katana/analysis/analysis_index.hpp"
#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/runtime_code_copy_analysis.hpp"
#include "katana/analysis/symbol_names.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
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

enum class GuardedAotEntryOrigin : std::uint8_t {
    IndirectCall,
    TailIngress,
    JumpTableTail,
    StaticReturn,
    StoredCodeAddress,
    ReturnedCodeAddressTable
};

struct GuardedAotEntry {
    std::uint32_t guest_address = 0u;
    // Diagnostic ownership hint only. The guest entry remains independently
    // dispatchable so its branch and delay-slot side effects are never skipped.
    std::uint32_t shared_body_address = 0u;
    ControlFlowEvidence evidence = ControlFlowEvidence::GuardedPartial;
    std::vector<GuardedAotEntryOrigin> origins;
    // Instruction/call sites that produced this guarded inventory entry.
    std::vector<std::uint32_t> source_sites;
    // Owning functions, backing tables or other non-instruction provenance.
    std::vector<std::uint32_t> source_objects;
    std::string source_identity;
    std::uint64_t source_byte_offset = 0u;
    std::uint32_t entry_byte_extent = 0u;
    std::string entry_byte_identity;
};

struct ControlFlowAnalysisResult {
    RecursiveAnalysisResult recursive;
    RuntimeCodeCopyAnalysis runtime_code_copies;
    std::vector<IndirectControlFlowResolution> indirect_control_flow;
    std::vector<StaticReturnContinuationCandidate> static_return_continuations;
    std::vector<JumpTableAnalysis> jump_tables;
    std::vector<FunctionValueSummary> function_value_summaries;
    std::vector<ResolvedControlFlowEdge> resolved_edges;
    std::vector<ControlFlowSite> sites;
    // Every accepted entry remains runtime-guarded, but code generation must
    // still provide a native entry block or a byte-bound native template.
    std::vector<GuardedAotEntry> guarded_aot_entries;
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
    std::size_t guarded_code_inventory_candidates = 0u;
    std::size_t guarded_code_inventory_budget = 0u;
    std::size_t guarded_code_shape_validation_work = 0u;
    std::size_t guarded_code_shape_validation_work_budget = 0u;
    std::size_t guarded_code_shape_budget_exceeded_candidates = 0u;
    bool candidate_inventory_truncated = false;
    bool returned_table_scan_truncated = false;
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
[[nodiscard]] const char*
guarded_aot_entry_origin_name(GuardedAotEntryOrigin origin) noexcept;

[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(const katana::io::ExecutableImage& image,
                     const AnalysisOverrides* overrides = nullptr);

[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(const katana::io::ExecutableImage& image,
                     const AnalysisOverrides* overrides,
                     const ControlFlowAnalysisProgressCallback& progress_callback);

} // namespace katana::analysis
