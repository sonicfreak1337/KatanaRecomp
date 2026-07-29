#pragma once

#include "katana/analysis/abi.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::analysis {

struct FunctionRegisterValueSummary {
    std::uint8_t register_index = 0u;
    bool complete = false;
    bool guarded = false;
    bool abi_preserved = false;
    bool may_alias_stack = true;
    // Inventory-only return provenance.  These fields never prove a static
    // control-flow edge; they only preserve guarded native-entry evidence
    // across an ordinary helper return.
    bool inventory_code_pointer = false;
    bool inventory_pc_relative_code_literal = false;
    std::vector<std::uint32_t> inventory_code_pointer_values;
    std::vector<std::uint32_t> inventory_pc_relative_code_literal_values;
    // Internal candidate-return slice dependency. It is not control-flow or
    // code-pointer evidence; it only decides whether a direct helper needs a
    // contextual summary instead of its already authoritative global summary.
    bool contextual_candidate_dependency = false;
    std::vector<std::uint32_t> values;
    std::vector<std::uint32_t> return_sites;
    std::vector<std::uint32_t> evidence_callees;
    std::string reason;

    bool operator==(const FunctionRegisterValueSummary&) const = default;
};

struct FunctionMemoryValueSummary {
    std::uint32_t address = 0u;
    bool complete = false;
    bool guarded = false;
    std::vector<std::uint32_t> values;

    bool operator==(const FunctionMemoryValueSummary&) const = default;
};

struct FunctionValueSummary {
    std::uint32_t function_address = 0u;
    std::vector<FunctionRegisterValueSummary> registers;
    bool memory_complete = false;
    std::vector<FunctionMemoryValueSummary> memory_values;

    bool operator==(const FunctionValueSummary&) const = default;
};

struct InterproceduralTargetResolution {
    std::uint32_t instruction_address = 0u;
    std::uint8_t register_index = 0u;
    bool call = false;
    std::vector<std::uint32_t> targets;
    std::vector<std::uint32_t> call_sites;
    std::vector<std::uint32_t> callees;
    bool guarded = false;
    bool complete = false;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::string reason;
};

// A finite code address stored through a non-stack 32-bit memory operation
// where either the value or destination retains known guest-call argument
// provenance.  The destination may remain symbolic (for example VBR-relative);
// this is only native-inventory evidence and never a concrete dispatch edge.
struct StoredCodeAddressCandidate {
    std::uint32_t target_address = 0u;
    bool complete = false;
    bool guarded = true;
    std::vector<std::uint32_t> store_instruction_addresses;
    std::vector<std::uint32_t> evidence_call_sites;
    std::vector<std::uint32_t> evidence_callees;

    bool operator==(const StoredCodeAddressCandidate&) const = default;
};

// A bounded initial-snapshot pointer table reached through a finite
// interprocedural return value and then used as the base of a 32-bit load.
// Entries are guarded native-inventory candidates only; the live load remains
// authoritative at runtime.
struct ReturnedCodeAddressTableCandidate {
    std::uint32_t table_address = 0u;
    std::vector<std::uint32_t> target_addresses;
    std::vector<std::uint32_t> load_instruction_addresses;
    std::vector<std::uint32_t> evidence_call_sites;
    std::vector<std::uint32_t> evidence_callees;
    bool scan_truncated = false;

    bool operator==(const ReturnedCodeAddressTableCandidate&) const = default;
};

enum class ForwardedStoreContextLimitReason : std::uint8_t {
    RootCallSites,
    ContextCount,
    ReevaluationCount,
};

// One bounded terminal capsule per resolution owner. It is analysis-only and
// exists solely to explain a fail-closed export; it never participates in the
// runtime product path or in candidate selection.
struct ForwardedStoreContextLimitDiagnostic {
    std::uint32_t owner_entry = 0u;
    std::uint32_t target = 0u;
    std::uint32_t exemplar_root_call_site = 0u;
    std::size_t context_count = 0u;
    std::size_t root_call_site_count = 0u;
    std::size_t evaluation_count = 0u;
    bool tail = false;
    bool isolated = false;
    ForwardedStoreContextLimitReason reason =
        ForwardedStoreContextLimitReason::ContextCount;

    bool operator==(const ForwardedStoreContextLimitDiagnostic&) const = default;
};

// Bounded inventory walks have several independent resource contracts. Keep
// their terminal evidence separate: an otherwise small candidate inventory
// must not be reported as if the 1,024-entry collector itself overflowed.
// Counts describe unique bounded units, never individual rejected attempts.
struct GuardedCodeInventoryWalkDiagnostics {
    std::size_t inventory_region_count = 0u;
    std::size_t inventory_region_budget = 0u;
    std::size_t pending_inventory_region_count = 0u;
    std::size_t inventory_region_block_budget = 0u;
    std::size_t inventory_region_block_limited_regions = 0u;
    std::size_t forwarded_store_context_budget = 0u;
    std::size_t forwarded_store_context_limited_functions = 0u;
    std::vector<ForwardedStoreContextLimitDiagnostic>
        forwarded_store_context_limit_diagnostics;
    std::size_t contextual_return_context_budget = 0u;
    std::size_t contextual_return_context_limited_functions = 0u;
    std::size_t contextual_return_evaluation_budget = 0u;
    std::size_t contextual_return_evaluation_limited_functions = 0u;
    std::size_t abi_stack_argument_slot_budget = 0u;
    std::size_t abi_stack_argument_projection_truncated_functions = 0u;

    [[nodiscard]] constexpr bool truncated() const noexcept {
        return pending_inventory_region_count != 0u ||
               inventory_region_block_limited_regions != 0u ||
               forwarded_store_context_limited_functions != 0u ||
               contextual_return_context_limited_functions != 0u ||
               contextual_return_evaluation_limited_functions != 0u ||
               abi_stack_argument_projection_truncated_functions != 0u;
    }

    bool operator==(const GuardedCodeInventoryWalkDiagnostics&) const = default;
};

// A larger, separately bounded native-code inventory channel.  Its entries
// never become fixed CFG edges; the live runtime value remains authoritative.
// The ordinary abstract-value domain intentionally retains its much smaller
// dataflow bound.
struct GuardedCodeInventory {
    std::vector<StoredCodeAddressCandidate> stored_code_addresses;
    std::vector<ReturnedCodeAddressTableCandidate> returned_code_address_tables;
    std::size_t raw_stored_candidate_budget = 0u;
    std::size_t raw_stored_candidate_count = 0u;
    std::size_t candidate_budget = 0u;
    std::size_t candidate_count = 0u;
    std::size_t shape_validation_work = 0u;
    std::size_t shape_validation_work_budget = 0u;
    std::size_t shape_budget_exceeded_candidates = 0u;
    bool raw_stored_candidates_truncated = false;
    bool candidate_budget_exhausted = false;
    bool candidate_inventory_truncated = false;
    bool table_scan_truncated = false;
    GuardedCodeInventoryWalkDiagnostics walk_diagnostics;
};

namespace detail {

enum class GuardedCodeInventoryPriorityKind : std::uint8_t {
    CompleteStored,
    IncompleteStored,
    CompleteReturnedTable,
    TruncatedReturnedTable,
};

struct GuardedCodeInventoryPriorityTarget {
    std::uint32_t target_address = 0u;
    GuardedCodeInventoryPriorityKind kind =
        GuardedCodeInventoryPriorityKind::IncompleteStored;
};

[[nodiscard]] std::vector<std::uint32_t>
guarded_code_inventory_priority_order(
    std::span<const GuardedCodeInventoryPriorityTarget> candidates,
    std::size_t returned_table_reserve);

} // namespace detail

struct FunctionValueAnalysisResult {
    std::vector<FunctionValueSummary> summaries;
    std::vector<InterproceduralTargetResolution> resolutions;
    GuardedCodeInventory guarded_code_inventory;
    std::size_t fixpoint_iterations = 0u;
    std::size_t strongly_connected_components = 0u;
    std::size_t unchanged_ingress_skips = 0u;
    std::size_t iteration_budget = 0u;
    bool budget_exhausted = false;
};

struct FunctionValueAnalysisProgress {
    std::string_view phase;
    std::size_t functions = 0u;
    std::size_t blocks = 0u;
    std::size_t fixpoint_iterations = 0u;
    std::size_t completed_functions = 0u;
    std::size_t pending = 0u;
    std::size_t resolutions = 0u;
};

using FunctionValueAnalysisProgressCallback =
    std::function<void(const FunctionValueAnalysisProgress& progress)>;

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const std::uint32_t> function_entries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges = {});

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const FunctionBoundary> function_boundaries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges = {});

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const std::uint32_t> function_entries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges,
                        const FunctionValueAnalysisProgressCallback& progress_callback);

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const FunctionBoundary> function_boundaries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges,
                        const FunctionValueAnalysisProgressCallback& progress_callback);

} // namespace katana::analysis
