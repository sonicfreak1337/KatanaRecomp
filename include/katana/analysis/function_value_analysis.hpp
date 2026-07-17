#pragma once

#include "katana/analysis/basic_blocks.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace katana::analysis {

struct FunctionRegisterValueSummary {
    std::uint8_t register_index = 0u;
    bool complete = false;
    bool abi_preserved = false;
    std::vector<std::uint32_t> values;
    std::vector<std::uint32_t> return_sites;
    std::vector<std::uint32_t> evidence_callees;
    std::string reason;

    bool operator==(const FunctionRegisterValueSummary&) const = default;
};

struct FunctionValueSummary {
    std::uint32_t function_address = 0u;
    std::vector<FunctionRegisterValueSummary> registers;

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
    std::string reason;
};

struct FunctionValueAnalysisResult {
    std::vector<FunctionValueSummary> summaries;
    std::vector<InterproceduralTargetResolution> resolutions;
    std::size_t fixpoint_iterations = 0u;
};

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const std::uint32_t> function_entries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges = {});

} // namespace katana::analysis
