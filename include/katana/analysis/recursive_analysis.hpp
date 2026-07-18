#pragma once

#include "katana/analysis/evidence.hpp"
#include "katana/analysis/symbol_names.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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
    JumpTableCall,
    UserOverride,
    UserHint,
    Symbol
};

enum class AnalysisConfidence { Low, Medium, High, Certain };

struct FunctionCandidate {
    std::uint32_t address = 0u;
    AnalysisConfidence confidence = AnalysisConfidence::Low;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::vector<FunctionOrigin> origins;
};

enum class AnalysisConflictKind { FunctionEntryInDelaySlot };

struct AnalysisConflict {
    std::uint32_t address = 0u;
    std::uint64_t size = 0u;
    AnalysisConflictKind kind = AnalysisConflictKind::FunctionEntryInDelaySlot;
};

struct AnalysisDiagnostic {
    std::uint32_t address = 0u;
    std::uint16_t opcode = 0u;
    std::string reason;
};

struct AnalysisSeed {
    std::uint32_t address = 0u;
    std::vector<FunctionOrigin> function_origins;
    bool guarded_candidate = false;
    ControlFlowEvidence evidence = ControlFlowEvidence::ProvenComplete;
};

struct ContextualInstruction {
    katana::sh4::DisassemblyLine line;
    std::uint32_t incoming_address = 0u;
    std::optional<std::uint32_t> delay_slot_owner;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
};

struct RecursiveAnalysisResult;

struct RecursiveAnalysisOptions {
    std::vector<AnalysisSeed> additional_seeds;
    const RecursiveAnalysisResult* baseline = nullptr;
};

struct RecursiveAnalysisResult {
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
};

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
