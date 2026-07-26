#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

struct ExternalIsaEvidenceCounts {
    std::uint64_t total = 0u;
    std::uint64_t applicable = 0u;
    std::uint64_t passed = 0u;
    std::uint64_t failed = 0u;
    std::uint64_t not_applicable = 0u;
};

struct ExternalIsaEvidence {
    std::string source;
    std::string katana_commit;
    std::string corpus_commit;
    std::string corpus_manifest_sha256;
    std::string backend_profile;
    std::uint32_t backend_profile_version = 0u;
    std::uint32_t runtime_abi = 0u;
    std::uint32_t backend_abi = 0u;
    std::string scope;
    bool complete_scope = false;
    std::uint64_t expected_scope_vectors = 0u;
    std::string memory_profile;
    std::string fpu_comparison_mode;
    ExternalIsaEvidenceCounts counts;
    std::uint64_t waiver_count = 0u;
    bool stale = true;
    std::vector<std::string> stale_reasons;
};

[[nodiscard]] IsaCoverageReport build_isa_coverage_report();
[[nodiscard]] std::string format_isa_coverage_report(const IsaCoverageReport& report);
[[nodiscard]] ExternalIsaEvidence parse_external_isa_evidence_json(std::string_view document);
[[nodiscard]] std::string
format_alpha_isa_json(const IsaCoverageReport& report,
                      const std::optional<ExternalIsaEvidence>& external_evidence = std::nullopt);
[[nodiscard]] const char* alpha_isa_support_name(AlphaIsaSupport support) noexcept;

} // namespace katana::sh4
