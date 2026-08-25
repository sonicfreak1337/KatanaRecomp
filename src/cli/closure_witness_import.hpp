#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace katana::cli {

inline constexpr std::string_view closure_witness_v5_schema =
    "katana-closure-witness-v5";
inline constexpr std::string_view closure_witness_v5_version = "5";

// All identities, generations, and addresses are strings: JSON numbers are
// not an identity-safe carrier for uint64_t.
struct ClosureBinding final {
    std::string analysis_artifact_key;
    std::string content_identity;
    std::string boot_byte_identity;
    std::string project_identity;
    std::string analysis_contract_identity;
    std::string image_analysis_key;
    std::string game_project_identity;
    std::string native_port_identity;
    std::string native_port_artifact_identity;
    std::string analysis_implementation_identity;
    std::string analysis_cache_implementation_identity;
    std::string ir_product_implementation_identity;
    std::string codegen_implementation_identity;
    std::string analyzer_abi;
    std::string backend_abi;
    std::string analysis_mode;
    std::string disc_volume_start_lba;
    std::string disc_extent_lba_bias;
    std::string runtime_generation;
};

struct ClosureReference final {
    std::string address;
    std::string identity;
};

struct ClosurePointer final {
    bool address_present = false;
    std::string address;
    std::string identity;
    std::string value;
};

struct ClosureSlot final {
    bool slot_present = false;
    std::string address;
    std::string identity;
};

struct ClosureWitnessFlags final {
    bool immutable = false;
    bool bounded = false;
    bool complete = false;
    bool runtime_observation = false;
    bool reproof_required = true;
};

struct ClosureWitness final {
    std::string kind;
    ClosureReference source;
    ClosureReference callsite;
    ClosurePointer pointer;
    ClosureReference target;
    ClosureReference alias;
    ClosureSlot slot;
    ClosureWitnessFlags flags;
};

struct ClosureWitnessDocument final {
    ClosureBinding binding;
    std::vector<ClosureWitness> witnesses;
    std::uint64_t drop_count = 0u;
    bool truncated = false;
    bool invalid = false;
};

struct ClosureWitnessImportLimits final {
    std::size_t max_document_bytes = 1u * 1024u * 1024u;
    std::size_t max_line_bytes = 64u * 1024u;
    std::size_t max_witnesses = 128u;
    std::size_t max_string_bytes = 4096u;
    std::size_t max_json_nodes = 8192u;
    std::size_t max_json_depth = 16u;
};

struct ClosureWitnessImportResult final {
    bool parsed = false;
    bool valid = false;
    bool closure_admitted = false;
    bool reproof_required = true;
    std::string error;
    ClosureWitnessDocument document;
};

[[nodiscard]] bool closure_witness_is_admissible(
    const ClosureWitnessDocument& document) noexcept;

[[nodiscard]] ClosureWitnessImportResult import_closure_witness_v5(
    std::string_view document,
    const ClosureWitnessImportLimits& limits = {});

[[nodiscard]] std::string serialize_closure_witness_v5(
    const ClosureWitnessDocument& document);

} // namespace katana::cli
