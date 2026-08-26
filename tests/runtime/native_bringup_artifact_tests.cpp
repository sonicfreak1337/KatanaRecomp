#include "katana/runtime/native_bringup_artifact.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using katana::runtime::NativeBringupAuthoringArtifact;
using katana::runtime::NativeBringupAuthoringDefinition;
using katana::runtime::NativeBringupEvidenceStage;
using katana::runtime::NativeBringupPromotionType;
using katana::runtime::NativeBringupTargetEvidence;
using katana::runtime::NativeBringupTransferKind;

constexpr std::string_view sha_a =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view sha_b =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view sha_c =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr std::string_view sha_d =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr std::string_view sha_e =
    "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
constexpr std::string_view sha_f =
    "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

void require(const bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "TEST FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

template <typename Function>
void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture artifact cannot be opened");
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    require(size > 0, "fixture artifact is empty");
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    require(static_cast<bool>(input), "fixture artifact cannot be read");
    return bytes;
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "mutated fixture cannot be written");
}

NativeBringupTargetEvidence proven();

NativeBringupTargetEvidence candidate() {
    auto result = proven();
    result.stage = NativeBringupEvidenceStage::Candidate;
    result.callsite = 0x8C001004u;
    result.target = 0x8C003000u;
    result.target_owner = 0x8C003000u;
    result.observation = "runtime target observed; observation is not proof";
    result.static_correlation =
        "exact execution bytes correlated; CFG completeness remains open";
    result.missing_proof = "closed-outgoing-transfer-proof";
    result.proposed_promotion = NativeBringupPromotionType::AnalyzerReproof;
    result.analyzer_path = "control-flow/indirect-dispatch";
    return result;
}

NativeBringupTargetEvidence proven() {
    NativeBringupTargetEvidence result;
    result.stage = NativeBringupEvidenceStage::Proven;
    result.transfer_kind = NativeBringupTransferKind::CallRegister;
    result.source_owner = 0x8C001000u;
    result.source_owner_size = 0x20u;
    result.source_block = 0x8C001000u;
    result.source_block_size = 8u;
    result.callsite = 0x8C001004u;
    result.continuation = 0x8C001008u;
    result.source_owner_code_identity = sha_c;
    result.source_block_code_identity = sha_b;
    result.callsite_code_identity = sha_d;
    result.target = 0x8C002000u;
    result.target_block_size = 8u;
    result.target_owner = 0x8C002000u;
    result.target_owner_size = 0x20u;
    result.target_block_code_identity = sha_e;
    result.target_owner_code_identity = sha_f;
    result.source_image_id = "primary";
    result.target_image_id = "primary";
    result.source_module_identity = sha_a;
    result.target_module_identity = sha_a;
    result.source_generation = 7u;
    result.target_generation = 7u;
    result.observation = "runtime observation retained as correlation only";
    result.static_correlation = "exact owner call delay target and bytes";
    result.proposed_promotion = NativeBringupPromotionType::StaticCompiledTarget;
    result.analyzer_path = "prepared-native-port-admission/program-index";
    return result;
}

NativeBringupAuthoringDefinition definition(
    const std::span<const NativeBringupTargetEvidence> targets) {
    return {katana::runtime::native_bringup_evidence_contract_version,
            "fixture-project",
            "fixture-v1",
            sha_a,
            sha_b,
            7u,
            targets};
}

} // namespace

int main() {
    const auto root = std::filesystem::current_path() /
                      "katana-native-bringup-artifact-fixture";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    };

    try {
        const std::array records{candidate(), proven()};
        const auto artifact_path = root / "allowlist.katana-native-bringup";
        const auto written = NativeBringupAuthoringArtifact::write(
            artifact_path, definition(records));
        const auto loaded = NativeBringupAuthoringArtifact::load(artifact_path);
        const std::array reversed_records{proven(), candidate()};
        const auto deterministic = NativeBringupAuthoringArtifact::write(
            root / "allowlist-reversed.katana-native-bringup",
            definition(reversed_records));
        require(written->artifact_identity() == loaded->artifact_identity() &&
                    written->artifact_identity() ==
                        deterministic->artifact_identity() &&
                    loaded->artifact_identity().starts_with("sha256:") &&
                    loaded->artifact_identity().size() == 71u &&
                    loaded->definition().analysis_identity == sha_a &&
                    loaded->definition().aot_pack_identity == sha_b &&
                    loaded->definition().aot_pack_generation == 7u &&
                    loaded->definition().targets.size() == records.size() &&
                    loaded->definition().targets[0].stage ==
                        NativeBringupEvidenceStage::Proven &&
                    loaded->definition().targets[0].source_block ==
                        0x8C001000u &&
                    loaded->definition().targets[0].source_block_size == 8u &&
                    loaded->definition().targets[0]
                            .source_block_code_identity == sha_b &&
                    loaded->definition().targets[1].stage ==
                        NativeBringupEvidenceStage::Candidate,
                "authoring artifact was not canonical or did not round-trip "
                "exact pack/evidence identity");
        require(
            !katana::runtime::native_bringup_stage_is_static_proof(
                NativeBringupEvidenceStage::Observed) &&
                !katana::runtime::native_bringup_stage_is_static_proof(
                    NativeBringupEvidenceStage::Candidate) &&
                katana::runtime::native_bringup_stage_is_static_proof(
                    NativeBringupEvidenceStage::Proven) &&
                !katana::runtime::native_bringup_stage_is_static_proof(
                    NativeBringupEvidenceStage::RuntimeContract) &&
                !katana::runtime::native_bringup_stage_is_static_proof(
                    NativeBringupEvidenceStage::Unresolved),
            "only Proven evidence may satisfy the strict proof class");

        auto invalid_definition = definition(records);
        invalid_definition.analysis_identity =
            "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    invalid_definition);
            },
            "non-canonical SHA identity was accepted");
        invalid_definition = definition(records);
        invalid_definition.aot_pack_generation = 0u;
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    invalid_definition);
            },
            "zero AOT-pack generation was accepted");
        auto wrong_generation = proven();
        ++wrong_generation.target_generation;
        const std::array wrong_generation_records{wrong_generation};
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    definition(wrong_generation_records));
            },
            "Proven target with a generation outside its AOT pack was accepted");
        auto nonterminal = proven();
        nonterminal.source_block_size += 2u;
        const std::array nonterminal_records{nonterminal};
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    definition(nonterminal_records));
            },
            "Proven callsite that is not the exact terminal transfer was accepted");
        auto unsafe_candidate = candidate();
        unsafe_candidate.source_block_code_identity = {};
        const std::array unsafe_candidate_records{unsafe_candidate};
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    definition(unsafe_candidate_records));
            },
            "Candidate without exact execution-safety identity was accepted");
        auto proofless_candidate = candidate();
        proofless_candidate.missing_proof = {};
        const std::array proofless_candidate_records{proofless_candidate};
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    definition(proofless_candidate_records));
            },
            "Candidate without an open strict-proof task was accepted");

        const std::array duplicate_records{candidate(), candidate()};
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    definition(duplicate_records));
            },
            "duplicate callsite/target record was accepted");
        auto conflicting = candidate();
        conflicting.target += 2u;
        conflicting.callsite_code_identity = sha_a;
        const std::array conflicting_records{candidate(), conflicting};
        require_failure(
            [&] {
                katana::runtime::validate_native_bringup_authoring_definition(
                    definition(conflicting_records));
            },
            "conflicting source authority at one callsite was accepted");

        const auto bytes = read_bytes(artifact_path);
        auto corrupt = bytes;
        corrupt.back() ^= 0x01u;
        const auto corrupt_path = root / "corrupt.katana-native-bringup";
        write_bytes(corrupt_path, corrupt);
        require_failure(
            [&] { static_cast<void>(NativeBringupAuthoringArtifact::load(corrupt_path)); },
            "corrupt payload was accepted");

        auto truncated = bytes;
        truncated.pop_back();
        const auto truncated_path = root / "truncated.katana-native-bringup";
        write_bytes(truncated_path, truncated);
        require_failure(
            [&] {
                static_cast<void>(
                    NativeBringupAuthoringArtifact::load(truncated_path));
            },
            "truncated artifact was accepted");

        auto trailing = bytes;
        trailing.push_back(0u);
        const auto trailing_path = root / "trailing.katana-native-bringup";
        write_bytes(trailing_path, trailing);
        require_failure(
            [&] { static_cast<void>(NativeBringupAuthoringArtifact::load(trailing_path)); },
            "artifact with trailing bytes was accepted");

        cleanup();
        std::cout << "native bring-up artifact tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        cleanup();
        std::cerr << "TEST FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
