#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace katana::sh4 {

struct IsaCoverageEntry {
    InstructionKind kind;
    std::string name;
    std::size_t encoding_rule_count;
    std::uint32_t decoded_opcode_count;
    bool contains_privileged_encoding;
};

struct IsaCoverageReport {
    std::vector<IsaCoverageEntry> instructions;
    std::uint32_t known_opcode_count = 0;
    std::uint32_t unknown_opcode_count = 0;
};

[[nodiscard]] IsaCoverageReport build_isa_coverage_report();
[[nodiscard]] std::string format_isa_coverage_report(const IsaCoverageReport& report);

}
