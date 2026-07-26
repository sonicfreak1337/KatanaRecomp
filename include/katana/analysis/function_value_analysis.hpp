#pragma once

#include "katana/analysis/basic_blocks.hpp"
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

    bool operator==(const ReturnedCodeAddressTableCandidate&) const = default;
};

struct FunctionValueAnalysisResult {
    std::vector<FunctionValueSummary> summaries;
    std::vector<InterproceduralTargetResolution> resolutions;
    std::vector<StoredCodeAddressCandidate> stored_code_address_candidates;
    std::vector<ReturnedCodeAddressTableCandidate> returned_code_address_table_candidates;
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
                        std::span<const std::uint32_t> function_entries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges,
                        const FunctionValueAnalysisProgressCallback& progress_callback);

} // namespace katana::analysis
