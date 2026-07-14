#pragma once

#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <vector>

namespace katana::analysis {

struct RecursiveAnalysisResult {
    std::vector<katana::sh4::DisassemblyLine> instructions;
};

[[nodiscard]] RecursiveAnalysisResult analyze_reachable_code(
    const katana::io::ExecutableImage& image
);

}
