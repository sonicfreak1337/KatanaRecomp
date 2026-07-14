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

[[nodiscard]] std::vector<ConstantTraceEntry> propagate_local_constants(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const RegisterConstants& initial = {}
);

}
