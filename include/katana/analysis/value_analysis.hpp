#pragma once

#include "katana/analysis/evidence.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::analysis {

struct RegisterConstants {
    std::array<std::optional<std::uint32_t>, 16> registers;
    std::array<std::string, 16> sources;
};

struct ConstantTraceEntry {
    std::uint32_t address = 0u;
    RegisterConstants before;
    RegisterConstants after;
};

struct RegisterValueObservation {
    std::uint32_t instruction_address = 0u;
    std::uint8_t register_index = 0u;
    std::optional<std::uint32_t> value;
    std::string source;
};

struct RegisterValueAnalysis {
    std::vector<ConstantTraceEntry> trace;
    std::vector<RegisterValueObservation> indirect_control_flow;
};

enum class IndirectControlFlowKind { Jump, Call };

enum class ResolutionStatus { Resolved, Guarded, Unresolved };

enum class IndirectControlFlowOriginClass : std::uint8_t {
    NotApplicable,
    Callback,
    Parameter,
    Stack,
    ObjectVTable,
    Table,
    UnboundedMemory,
    RuntimePointer
};

[[nodiscard]] const char*
indirect_control_flow_origin_class_name(IndirectControlFlowOriginClass origin) noexcept;

[[nodiscard]] std::uint16_t
general_register_write_mask(const katana::sh4::DecodedInstruction& instruction) noexcept;

struct IndirectControlFlowResolution {
    std::uint32_t instruction_address = 0u;
    IndirectControlFlowKind kind = IndirectControlFlowKind::Jump;
    std::uint8_t register_index = 0u;
    ResolutionStatus status = ResolutionStatus::Unresolved;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    IndirectControlFlowOriginClass origin_class = IndirectControlFlowOriginClass::NotApplicable;
    std::vector<AnalysisEvidenceOrigin> evidence_origins;
    std::optional<std::uint32_t> target;
    std::string reason;
    std::vector<std::uint32_t> targets;
    std::vector<std::uint32_t> evidence_call_sites;
    std::vector<std::uint32_t> evidence_callees;
    std::string value_source;
    std::vector<std::uint32_t> definition_sites;
    bool definition_complete = false;
    bool preceding_call = false;
    katana::sh4::InstructionKind instruction_kind = katana::sh4::InstructionKind::Unknown;
    std::vector<std::uint32_t> analysis_candidates;
};

// Loading PR from a statically known value does not prove which later RTS will
// consume it.  It does, however, provide a bounded native-code candidate that
// can be compiled ahead of time while the live PR remains authoritative.
struct StaticReturnContinuationCandidate {
    std::uint32_t instruction_address = 0u;
    std::uint8_t register_index = 0u;
    std::uint32_t target_address = 0u;
    ControlFlowEvidence evidence = ControlFlowEvidence::RuntimeOnly;
    std::vector<AnalysisEvidenceOrigin> evidence_origins;
    std::string reason;
    std::string value_source;
};

struct LocalControlFlowAnalysis {
    std::vector<IndirectControlFlowResolution> indirect_control_flow;
    std::vector<StaticReturnContinuationCandidate> static_return_continuations;
};

[[nodiscard]] std::vector<ConstantTraceEntry>
propagate_local_constants(std::span<const katana::sh4::DisassemblyLine> lines,
                          const RegisterConstants& initial = {});

[[nodiscard]] std::vector<ConstantTraceEntry>
propagate_local_constants(std::span<const katana::sh4::DisassemblyLine> lines,
                          const katana::io::ExecutableImage& image,
                          const RegisterConstants& initial = {});

[[nodiscard]] RegisterValueAnalysis
analyze_register_values(std::span<const katana::sh4::DisassemblyLine> lines,
                        const RegisterConstants& initial = {});

[[nodiscard]] std::vector<IndirectControlFlowResolution>
resolve_indirect_control_flow(std::span<const katana::sh4::DisassemblyLine> lines,
                              const katana::io::ExecutableImage& image);

[[nodiscard]] LocalControlFlowAnalysis
analyze_local_control_flow(std::span<const katana::sh4::DisassemblyLine> lines,
                           const katana::io::ExecutableImage& image);

} // namespace katana::analysis
