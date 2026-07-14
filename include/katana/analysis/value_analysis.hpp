#pragma once

#include "katana/sh4/disassembler.hpp"
#include "katana/io/executable_image.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::analysis {

struct RegisterConstants {
    std::array<std::optional<std::uint32_t>, 16> registers;
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
};

struct RegisterValueAnalysis {
    std::vector<ConstantTraceEntry> trace;
    std::vector<RegisterValueObservation> indirect_control_flow;
};

enum class IndirectControlFlowKind {
    Jump,
    Call
};

enum class ResolutionStatus {
    Resolved,
    Unresolved
};

struct IndirectControlFlowResolution {
    std::uint32_t instruction_address = 0u;
    IndirectControlFlowKind kind = IndirectControlFlowKind::Jump;
    std::uint8_t register_index = 0u;
    ResolutionStatus status = ResolutionStatus::Unresolved;
    std::optional<std::uint32_t> target;
    std::string reason;
};

[[nodiscard]] std::vector<ConstantTraceEntry> propagate_local_constants(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const RegisterConstants& initial = {}
);

[[nodiscard]] RegisterValueAnalysis analyze_register_values(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const RegisterConstants& initial = {}
);

[[nodiscard]] std::vector<IndirectControlFlowResolution> resolve_indirect_control_flow(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::io::ExecutableImage& image
);

}
