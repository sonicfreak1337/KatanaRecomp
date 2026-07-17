#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace katana::sh4 {

inline constexpr std::uint32_t alpha_isa_contract_version = 1u;

enum class AlphaIsaSupport : std::uint8_t { Supported, Restricted, Rejected };

struct AlphaIsaLayerSupport {
    AlphaIsaSupport decoder = AlphaIsaSupport::Rejected;
    AlphaIsaSupport ir = AlphaIsaSupport::Rejected;
    AlphaIsaSupport backend = AlphaIsaSupport::Rejected;
    AlphaIsaSupport runtime = AlphaIsaSupport::Rejected;
};

struct AlphaIsaFamilyEntry {
    std::string id;
    std::string name;
    AlphaIsaSupport support = AlphaIsaSupport::Rejected;
    AlphaIsaLayerSupport layers;
    std::string semantic_contract;
    std::string limitation;
    std::string test_requirement;
};

struct IsaCoverageEntry {
    InstructionKind kind;
    std::string name;
    std::size_t encoding_rule_count;
    std::uint32_t decoded_opcode_count;
    bool contains_privileged_encoding;
    std::string family_id;
    AlphaIsaLayerSupport layers;
    AlphaIsaSupport support = AlphaIsaSupport::Rejected;
    std::string limitation;
    std::string test_requirement;
};

[[nodiscard]] AlphaIsaSupport alpha_isa_intersection(AlphaIsaLayerSupport layers) noexcept;

struct IsaCoverageReport {
    std::vector<AlphaIsaFamilyEntry> families;
    std::vector<IsaCoverageEntry> instructions;
    std::uint32_t known_opcode_count = 0;
    std::uint32_t unknown_opcode_count = 0;
};

[[nodiscard]] IsaCoverageReport build_isa_coverage_report();
[[nodiscard]] std::string format_isa_coverage_report(const IsaCoverageReport& report);
[[nodiscard]] std::string format_alpha_isa_json(const IsaCoverageReport& report);
[[nodiscard]] const char* alpha_isa_support_name(AlphaIsaSupport support) noexcept;

} // namespace katana::sh4
