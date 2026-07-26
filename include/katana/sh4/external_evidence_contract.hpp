#pragma once

#include <cstdint>
#include <string_view>

namespace katana::sh4::external_evidence_contract {

inline constexpr std::uint32_t version = 1u;
inline constexpr std::string_view source = "SingleStepTests/sh4";
inline constexpr std::string_view source_schema = "katana-sh4-sst-conformance";
inline constexpr std::string_view source_report_type = "sh4-sst-conformance";
inline constexpr std::uint32_t source_report_version = 1u;
inline constexpr std::string_view corpus_commit = "48975cb1a9569abb5a0cba587013ea54edf79100";
inline constexpr std::string_view corpus_manifest_sha256 =
    "155ddb446f00e6e4985ea0bb978cef8984e7835c864134b33d99e33af47b46c7";
inline constexpr std::uint64_t smoke_vector_count = 65u;
inline constexpr std::uint64_t full_vector_count = 116500u;

} // namespace katana::sh4::external_evidence_contract
