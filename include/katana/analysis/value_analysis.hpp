#pragma once

#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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

[[nodiscard]] std::vector<ConstantTraceEntry> propagate_local_constants(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const RegisterConstants& initial = {}
);

[[nodiscard]] RegisterValueAnalysis analyze_register_values(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const RegisterConstants& initial = {}
);

}
