#pragma once

#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstdint>
#include <vector>

namespace katana::analysis {

enum class DiscoveredByteKind {
    Unknown,
    Code,
    Data
};

struct ClassifiedRange {
    std::uint32_t start_address = 0u;
    std::uint64_t size = 0u;
    DiscoveredByteKind kind = DiscoveredByteKind::Unknown;
};

struct RecursiveAnalysisResult {
    std::vector<katana::sh4::DisassemblyLine> instructions;
    std::vector<ClassifiedRange> ranges;
};

[[nodiscard]] RecursiveAnalysisResult analyze_reachable_code(
    const katana::io::ExecutableImage& image
);

[[nodiscard]] const char* discovered_byte_kind_name(DiscoveredByteKind kind) noexcept;

}
