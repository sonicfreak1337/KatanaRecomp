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

enum class FunctionOrigin {
    EntryPoint,
    DirectCall,
    Symbol
};

enum class AnalysisConfidence {
    Low,
    Medium,
    High,
    Certain
};

struct FunctionCandidate {
    std::uint32_t address = 0u;
    AnalysisConfidence confidence = AnalysisConfidence::Low;
    std::vector<FunctionOrigin> origins;
};

struct RecursiveAnalysisResult {
    std::vector<katana::sh4::DisassemblyLine> instructions;
    std::vector<ClassifiedRange> ranges;
    std::vector<FunctionCandidate> functions;
};

[[nodiscard]] RecursiveAnalysisResult analyze_reachable_code(
    const katana::io::ExecutableImage& image
);

[[nodiscard]] const char* discovered_byte_kind_name(DiscoveredByteKind kind) noexcept;
[[nodiscard]] const char* function_origin_name(FunctionOrigin origin) noexcept;
[[nodiscard]] const char* analysis_confidence_name(AnalysisConfidence confidence) noexcept;

}
