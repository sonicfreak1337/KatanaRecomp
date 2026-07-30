#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/ir/ir.hpp"
#include "katana/sh4/disassembler.hpp"

#include <span>
#include <vector>

namespace katana::ir {

[[nodiscard]] Operation
lowering_operation_for_instruction(katana::sh4::InstructionKind kind) noexcept;

// Lowers exactly one decoded instruction context. This deliberately does not
// attach analysis-derived indirect targets; callers that validate serialized
// IR can therefore compare every source-derived field against current bytes
// before accepting the analysis-specific graph metadata.
[[nodiscard]] Instruction
lower_instruction(const katana::sh4::DisassemblyLine& line);

[[nodiscard]] Function
lower_function(std::span<const katana::sh4::DisassemblyLine> lines,
               const katana::analysis::FunctionInfo& function,
               std::span<const katana::analysis::ResolvedControlFlowEdge> resolved_edges = {},
               std::span<const std::uint32_t> function_entries = {});

[[nodiscard]] std::vector<Function>
lower_program(std::span<const katana::sh4::DisassemblyLine> lines,
              std::span<const katana::analysis::FunctionInfo> functions,
              std::span<const katana::analysis::ResolvedControlFlowEdge> resolved_edges = {});

// Native product code may leave its owner immediately after an architectural
// state change to accept an interrupt. These continuations are execution
// entries only: they split IR blocks without becoming function evidence or
// manufactured CFG edges.
[[nodiscard]] std::vector<std::uint32_t>
architectural_safepoint_block_leaders(
    const katana::analysis::ControlFlowAnalysisResult& analysis);

// Additional leaders split existing functions without becoming discovery
// seeds. Callers must not place one at an instruction marked only as a delay
// slot; an owner/slot pair is an atomic basic-block terminator.
[[nodiscard]] std::vector<Function>
lower_program(const katana::analysis::ControlFlowAnalysisResult& analysis,
              std::span<const std::uint32_t> additional_block_leaders = {});

} // namespace katana::ir
