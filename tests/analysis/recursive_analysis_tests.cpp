#include "katana/analysis/analysis_index.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace katana::io;
    Sha256Accumulator streamed_sha256;
    streamed_sha256.update("recursive-");
    streamed_sha256.update("");
    streamed_sha256.update("baseline");
    require(streamed_sha256.finish() ==
                sha256_bytes("recursive-baseline"),
            "Der gestreamte Baseline-Hash weicht vom kanonischen SHA-256 ab.");

    ExecutableImage image("recursive-fixture.bin");
    image.add_segment({".text",
                       0x8C010000u,
                       0u,
                       14u,
                       SegmentKind::Code,
                       {true, false, true},
                       {0x02u,
                        0xB0u,
                        0x09u,
                        0x00u,
                        0x0Bu,
                        0x00u,
                        0x09u,
                        0x00u,
                        0x0Bu,
                        0x00u,
                        0x09u,
                        0x00u,
                        0xFFu,
                        0xFFu}});
    image.add_segment(
        {".data", 0x8C020000u, 14u, 4u, SegmentKind::Data, {true, true, false}, {1u, 2u, 3u, 4u}});
    image.add_segment({".mystery",
                       0x8C030000u,
                       18u,
                       4u,
                       SegmentKind::Unknown,
                       {true, false, false},
                       {5u, 6u, 7u, 8u}});
    image.add_entry_point(0x8C010000u);
    image.add_symbol({"subroutine", 0x8C010008u, 4u, SymbolKind::Function, SymbolBinding::Global});

    const auto result = katana::analysis::analyze_reachable_code(image);
    require(result.instructions.size() == 6u,
            "Die Worklist hat nicht genau den erreichbaren Code entdeckt.");
    require(result.instructions.front().address == 0x8C010000u, "Der Einstiegspunkt fehlt.");
    require(result.instructions[1].is_delay_slot, "Der Call-Delay-Slot wurde nicht markiert.");
    require(result.instructions[4].address == 0x8C010008u,
            "Das direkte Callziel wurde nicht verfolgt.");
    require(result.instructions.back().address == 0x8C01000Au,
            "Der Delay-Slot des Callziels fehlt.");

    for (const auto& line : result.instructions) {
        require(line.address != 0x8C01000Cu, "Nicht erreichbare Bytes wurden linear dekodiert.");
    }
    require(result.ranges.size() == 4u, "Die Klassifikationsbereiche wurden nicht normalisiert.");
    require(result.ranges[0].start_address == 0x8C010000u && result.ranges[0].size == 12u &&
                result.ranges[0].kind == katana::analysis::DiscoveredByteKind::Code,
            "Erreichbarer Code wurde falsch klassifiziert.");
    require(result.ranges[1].start_address == 0x8C01000Cu && result.ranges[1].size == 2u &&
                result.ranges[1].kind == katana::analysis::DiscoveredByteKind::Unknown,
            "Nicht erreichbarer Codebereich wurde nicht als unknown erhalten.");
    require(result.ranges[2].kind == katana::analysis::DiscoveredByteKind::Data,
            "Datensegment wurde falsch klassifiziert.");
    require(result.ranges[3].kind == katana::analysis::DiscoveredByteKind::Unknown,
            "Unknown-Segment wurde falsch klassifiziert.");
    require(result.unreachable_code.size() == 1u &&
                result.unreachable_code[0].start_address == 0x8C01000Cu &&
                result.unreachable_code[0].size == 2u,
            "Der nicht erreichbare committed Codebereich wurde falsch ermittelt.");
    require(std::string(katana::analysis::discovered_byte_kind_name(result.ranges[2].kind)) ==
                "data",
            "Klassifikationsname ist instabil.");
    require(result.functions.size() == 2u, "Funktionskandidaten wurden nicht zusammengefuehrt.");
    require(result.functions[0].address == 0x8C010000u &&
                result.functions[0].confidence == katana::analysis::AnalysisConfidence::Certain &&
                result.functions[0].origins ==
                    std::vector<katana::analysis::FunctionOrigin>{
                        katana::analysis::FunctionOrigin::EntryPoint},
            "Einstiegspunkt-Herkunft oder Konfidenz ist falsch.");
    require(result.functions[1].address == 0x8C010008u &&
                result.functions[1].confidence == katana::analysis::AnalysisConfidence::High &&
                result.functions[1].origins.size() == 2u,
            "Call- und Symbolherkunft wurden nicht kombiniert.");
    require(std::string(katana::analysis::function_origin_name(result.functions[1].origins[0])) ==
                    "direct-call" &&
                std::string(katana::analysis::analysis_confidence_name(
                    result.functions[0].confidence)) == "certain",
            "Herkunfts- oder Konfidenzname ist instabil.");

    katana::analysis::RecursiveAnalysisOptions guarded_options;
    guarded_options.additional_seeds.push_back(
        {0x8C01000Cu, {katana::analysis::FunctionOrigin::GuardedSnapshot}});
    const auto guarded_result = katana::analysis::analyze_reachable_code(image, guarded_options);
    auto incremental_options = guarded_options;
    incremental_options.baseline = &result;
    const auto incremental_result =
        katana::analysis::analyze_reachable_code(image, incremental_options);
    require(incremental_result.instructions.size() == guarded_result.instructions.size() &&
                incremental_result.functions.size() == guarded_result.functions.size() &&
                incremental_result.reused_contexts == result.contextual_instructions.size() &&
                incremental_result.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::Reused &&
                incremental_result.processed_work_items < guarded_result.processed_work_items,
            "Delta-Seed analysiert die bekannte Ganzprogrammfront erneut.");

    // The private CFA session binds snapshots to both a stable owner token
    // and the exact immutable image revision. Old epochs remain usable after
    // later session epochs, but never through a different session/image or
    // after an in-place image mutation.
    {
        const auto throws_invalid_argument = [](auto&& operation) {
            try {
                operation();
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        katana::analysis::detail::RecursiveAnalysisSession session;
        katana::analysis::detail::RecursiveAnalysisDeltaJournal cold_delta;
        cold_delta.complete_seed_contract_supplied = true;
        const auto first_snapshot = session.analyze(
            image, std::span<const katana::analysis::AnalysisSeed>{},
            cold_delta);
        require(first_snapshot.valid() &&
                    !first_snapshot.cold_retry_required(),
                "Vollstaendiger Session-Kaltstart lieferte keinen Snapshot.");

        const std::array extra_seeds{
            katana::analysis::AnalysisSeed{
                0x8C01000Cu,
                {katana::analysis::FunctionOrigin::GuardedSnapshot},
                true,
                katana::analysis::ControlFlowEvidence::GuardedPartial}};
        katana::analysis::detail::RecursiveAnalysisDeltaJournal warm_delta;
        warm_delta.changed_seeds = extra_seeds;
        const auto second_snapshot = session.analyze(
            image, std::span<const katana::analysis::AnalysisSeed>{},
            warm_delta);
        const auto old_epoch_result = session.materialize(
            image, first_snapshot);
        require(second_snapshot.valid() &&
                    second_snapshot.epoch_version() >
                        first_snapshot.epoch_version() &&
                    old_epoch_result.instructions.size() ==
                        result.instructions.size(),
                "Ein unveraenderter alter Session-Snapshot verlor seine "
                "Lebensdauer nach einer neueren Epoch.");

        auto moved_session = std::move(session);
        const auto moved_owner_result = moved_session.materialize(
            image, first_snapshot);
        require(moved_owner_result.instructions.size() ==
                    old_epoch_result.instructions.size(),
                "Session-Move invalidierte den stabilen Snapshot-Owner.");
        katana::analysis::detail::RecursiveAnalysisSession foreign_session;
        require(throws_invalid_argument([&] {
                    static_cast<void>(foreign_session.materialize(
                        image, first_snapshot));
                }),
                "Fremde Session akzeptierte einen Snapshot-Owner.");

        ExecutableImage foreign_image("recursive-fixture.bin");
        foreign_image.add_segment({".text",
                                   0x8C010000u,
                                   0u,
                                   4u,
                                   SegmentKind::Code,
                                   {true, false, true},
                                   {0x0Bu, 0x00u, 0x09u, 0x00u}});
        foreign_image.add_entry_point(0x8C010000u);
        require(throws_invalid_argument([&] {
                    static_cast<void>(moved_session.materialize(
                        foreign_image, first_snapshot));
                }),
                "Snapshot wurde mit einer fremden Image-Identitaet "
                "materialisiert.");

        ExecutableImage revision_image("recursive-revision.bin");
        revision_image.add_segment({".text",
                                    0u,
                                    0u,
                                    8u,
                                    SegmentKind::Code,
                                    {true, false, true},
                                    {0x0Bu, 0x00u, 0x09u, 0x00u,
                                     0x0Bu, 0x00u, 0x09u, 0x00u}});
        revision_image.add_entry_point(0u);
        const std::array complete_seed_contract{
            katana::analysis::AnalysisSeed{
                4u,
                {katana::analysis::FunctionOrigin::StoredCodeAddress},
                false,
                katana::analysis::ControlFlowEvidence::ProvenComplete}};
        katana::analysis::detail::RecursiveAnalysisSession revision_session;
        const auto revision_snapshot = revision_session.analyze(
            revision_image, complete_seed_contract, cold_delta);
        revision_image.write_u32_le(0u, 0x00090009u);
        require(throws_invalid_argument([&] {
                    static_cast<void>(revision_session.materialize(
                        revision_image, revision_snapshot));
                }),
                "Snapshot wurde mit einer revidierten Image-Payload "
                "materialisiert.");

        katana::analysis::detail::RecursiveAnalysisDeltaJournal
            optimistic_delta;
        const auto retry = revision_session.analyze(
            revision_image,
            std::span<const katana::analysis::AnalysisSeed>{},
            optimistic_delta);
        require(retry.cold_retry_required() && !retry.valid() &&
                    retry.epoch_version() ==
                        revision_snapshot.epoch_version() &&
                    retry.baseline_status() ==
                        katana::analysis::RecursiveAnalysisBaselineStatus::
                            ImageRevisionMismatch,
                "Unerwarteter Cold-Fallback publizierte einen partiellen "
                "Delta-Snapshot statt eines typisierten Retries.");
        const auto rebuilt_snapshot = revision_session.analyze(
            revision_image, complete_seed_contract, cold_delta);
        const auto rebuilt_result = revision_session.materialize(
            revision_image, rebuilt_snapshot);
        require(rebuilt_snapshot.valid() &&
                    std::any_of(rebuilt_result.functions.begin(),
                                rebuilt_result.functions.end(),
                                [](const auto& function) {
                                    return function.address == 4u;
                                }),
                "Cold-Retry rekonstruierte den vollstaendigen Seedvertrag "
                "nicht.");
    }

    // A RecursiveAnalysisResult is a public baseline input. Exercise every
    // fail-safe binding dimension here so stale code can never be laundered
    // into a complete CFG merely because guest addresses happen to match.
    {
        const auto same_semantics = [](const auto& left,
                                       const auto& right) {
            const auto same_line = [](const auto& lhs,
                                      const auto& rhs) {
                return lhs.address == rhs.address &&
                       lhs.opcode == rhs.opcode &&
                       lhs.is_delay_slot == rhs.is_delay_slot &&
                       lhs.target_address == rhs.target_address;
            };
            if (left.instructions.size() != right.instructions.size() ||
                left.contextual_instructions.size() !=
                    right.contextual_instructions.size() ||
                left.proven_instruction_addresses !=
                    right.proven_instruction_addresses ||
                left.guarded_candidate_instruction_addresses !=
                    right.guarded_candidate_instruction_addresses ||
                left.limit != right.limit)
                return false;
            for (std::size_t index = 0u;
                 index < left.instructions.size(); ++index) {
                const auto& lhs = left.instructions[index];
                const auto& rhs = right.instructions[index];
                if (!same_line(lhs, rhs))
                    return false;
            }
            for (std::size_t index = 0u;
                 index < left.contextual_instructions.size(); ++index) {
                const auto& lhs = left.contextual_instructions[index];
                const auto& rhs = right.contextual_instructions[index];
                if (!same_line(lhs.line, rhs.line) ||
                    lhs.incoming_address != rhs.incoming_address ||
                    lhs.delay_slot_owner != rhs.delay_slot_owner ||
                    lhs.evidence != rhs.evidence)
                    return false;
            }
            if (left.functions.size() != right.functions.size()) return false;
            for (std::size_t index = 0u;
                 index < left.functions.size(); ++index) {
                if (left.functions[index].address !=
                        right.functions[index].address ||
                    left.functions[index].size !=
                        right.functions[index].size)
                    return false;
            }
            auto normalized_left = left;
            auto normalized_right = right;
            normalized_left.baseline_status =
                katana::analysis::RecursiveAnalysisBaselineStatus::
                    NotRequested;
            normalized_right.baseline_status =
                katana::analysis::RecursiveAnalysisBaselineStatus::
                    NotRequested;
            return katana::analysis::format_recursive_analysis_report(
                       normalized_left) ==
                   katana::analysis::format_recursive_analysis_report(
                       normalized_right);
        };
        const auto make_baseline_image = [](const bool leading_nop) {
            ExecutableImage candidate;
            candidate.add_segment(
                {".baseline",
                 0u,
                 0u,
                 12u,
                 SegmentKind::Code,
                 {true, true, true},
                 leading_nop
                     ? std::vector<std::uint8_t>{
                           0x09u, 0x00u, 0x0Bu, 0x00u,
                           0x09u, 0x00u, 0x09u, 0x00u,
                           0x0Bu, 0x00u, 0x09u, 0x00u}
                     : std::vector<std::uint8_t>{
                           0x0Bu, 0x00u, 0x09u, 0x00u,
                           0x0Bu, 0x00u, 0x09u, 0x00u,
                           0x0Bu, 0x00u, 0x09u, 0x00u}});
            candidate.add_entry_point(0u);
            return candidate;
        };
        auto first_image = make_baseline_image(false);
        const auto first =
            katana::analysis::analyze_reachable_code(first_image);

        auto foreign_image = make_baseline_image(true);
        katana::analysis::RecursiveAnalysisOptions foreign_options;
        foreign_options.baseline = &first;
        const auto foreign = katana::analysis::analyze_reachable_code(
            foreign_image, foreign_options);
        const auto foreign_fresh =
            katana::analysis::analyze_reachable_code(foreign_image);
        require(
            foreign.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        ImageIdentityMismatch &&
                foreign.reused_contexts == 0u &&
                !foreign.instructions.empty() &&
                foreign.instructions.front().opcode == 0x0009u &&
                same_semantics(foreign, foreign_fresh),
            "Eine fremde Image-Baseline lieferte alte Opcodes bei gleichen "
            "Gastadressen.");

        auto mutated_image = make_baseline_image(false);
        const auto before_mutation =
            katana::analysis::analyze_reachable_code(mutated_image);
        mutated_image.write_u32_le(0u, 0x000B0009u);
        katana::analysis::RecursiveAnalysisOptions mutation_options;
        mutation_options.baseline = &before_mutation;
        const auto after_mutation =
            katana::analysis::analyze_reachable_code(
                mutated_image, mutation_options);
        const auto mutation_fresh =
            katana::analysis::analyze_reachable_code(mutated_image);
        require(
            after_mutation.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        ImageRevisionMismatch &&
                after_mutation.reused_contexts == 0u &&
                !after_mutation.instructions.empty() &&
                after_mutation.instructions.front().opcode == 0x0009u &&
                same_semantics(after_mutation, mutation_fresh),
            "Eine nach Image-Mutation stale Baseline wurde wiederverwendet.");

        auto seed_image = make_baseline_image(false);
        katana::analysis::RecursiveAnalysisOptions seeded_options;
        seeded_options.additional_seeds.push_back(
            {4u,
             {katana::analysis::FunctionOrigin::StoredCodeAddress},
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial});
        const auto seeded = katana::analysis::analyze_reachable_code(
            seed_image, seeded_options);
        katana::analysis::RecursiveAnalysisOptions removed_seed_options;
        removed_seed_options.baseline = &seeded;
        const auto removed_seed =
            katana::analysis::analyze_reachable_code(
                seed_image, removed_seed_options);
        const auto unseeded =
            katana::analysis::analyze_reachable_code(seed_image);
        const auto use_baseline = [&](const auto& candidate) {
            katana::analysis::RecursiveAnalysisOptions options;
            options.baseline = &candidate;
            return katana::analysis::analyze_reachable_code(
                seed_image, options);
        };
        const auto valid_copy = unseeded;
        const auto valid_copy_reused = use_baseline(valid_copy);
        auto tampered_instruction = unseeded;
        tampered_instruction.instructions.front().opcode = 0xFFFFu;
        tampered_instruction.instructions.front().instruction =
            katana::sh4::decode(0xFFFFu);
        const auto tampered_instruction_rejected =
            use_baseline(tampered_instruction);
        auto tampered_context = unseeded;
        tampered_context.contextual_instructions.front().incoming_address ^=
            2u;
        const auto tampered_context_rejected =
            use_baseline(tampered_context);
        auto tampered_function = unseeded;
        tampered_function.functions.front().confidence =
            katana::analysis::AnalysisConfidence::Low;
        const auto tampered_function_rejected =
            use_baseline(tampered_function);
        const auto use_seeded_baseline = [&](const auto& candidate) {
            auto options = seeded_options;
            options.baseline = &candidate;
            return katana::analysis::analyze_reachable_code(
                seed_image, options);
        };
        auto duplicated_seed_origin = seeded;
        duplicated_seed_origin.seed_contract.front()
            .function_origins.push_back(
                katana::analysis::FunctionOrigin::StoredCodeAddress);
        const auto duplicated_seed_origin_rejected =
            use_seeded_baseline(duplicated_seed_origin);
        auto invalid_seed_evidence = seeded;
        invalid_seed_evidence.seed_contract.front()
            .decode_evidences.front() =
                static_cast<katana::analysis::ControlFlowEvidence>(0xFFu);
        const auto invalid_seed_evidence_rejected =
            use_seeded_baseline(invalid_seed_evidence);
        auto multi_seed_options = seeded_options;
        multi_seed_options.additional_seeds.push_back(
            {8u,
             {katana::analysis::FunctionOrigin::UserHint},
             true,
             katana::analysis::ControlFlowEvidence::HintCandidate});
        const auto multi_seeded =
            katana::analysis::analyze_reachable_code(
                seed_image, multi_seed_options);
        auto unsorted_seed_contract = multi_seeded;
        std::swap(unsorted_seed_contract.seed_contract[0],
                  unsorted_seed_contract.seed_contract[1]);
        multi_seed_options.baseline = &unsorted_seed_contract;
        const auto unsorted_seed_contract_rejected =
            katana::analysis::analyze_reachable_code(
                seed_image, multi_seed_options);

        ExecutableImage diagnostic_seal_image;
        diagnostic_seal_image.add_segment(
            {".diagnostic-seal",
             0u,
             0u,
             2u,
             SegmentKind::Code,
             {true, false, true},
             {0xFFu, 0xFFu}});
        diagnostic_seal_image.add_entry_point(0u);
        const auto diagnostic_sealed =
            katana::analysis::analyze_reachable_code(
                diagnostic_seal_image);
        auto tampered_diagnostic = diagnostic_sealed;
        tampered_diagnostic.diagnostics.front().reason =
            "tampered-diagnostic";
        katana::analysis::RecursiveAnalysisOptions
            tampered_diagnostic_options;
        tampered_diagnostic_options.baseline = &tampered_diagnostic;
        const auto tampered_diagnostic_rejected =
            katana::analysis::analyze_reachable_code(
                diagnostic_seal_image,
                tampered_diagnostic_options);
        require(
            removed_seed.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        SeedContractMismatch &&
                removed_seed.reused_contexts == 0u &&
                std::none_of(
                    removed_seed.instructions.begin(),
                    removed_seed.instructions.end(),
                    [](const auto& line) { return line.address == 4u; }) &&
                same_semantics(removed_seed, unseeded) &&
                valid_copy_reused.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::Reused &&
                same_semantics(valid_copy_reused, unseeded) &&
                tampered_instruction_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        PayloadMismatch &&
                tampered_context_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        PayloadMismatch &&
                tampered_function_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        PayloadMismatch &&
                tampered_diagnostic_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        PayloadMismatch &&
                duplicated_seed_origin_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        PayloadMismatch &&
                invalid_seed_evidence_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        PayloadMismatch &&
                unsorted_seed_contract_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        PayloadMismatch &&
                tampered_instruction_rejected.reused_contexts == 0u &&
                tampered_context_rejected.reused_contexts == 0u &&
                tampered_function_rejected.reused_contexts == 0u &&
                tampered_diagnostic_rejected.reused_contexts == 0u &&
                duplicated_seed_origin_rejected.reused_contexts == 0u &&
                invalid_seed_evidence_rejected.reused_contexts == 0u &&
                unsorted_seed_contract_rejected.reused_contexts == 0u &&
                same_semantics(tampered_instruction_rejected, unseeded) &&
                same_semantics(tampered_context_rejected, unseeded) &&
                same_semantics(tampered_function_rejected, unseeded) &&
                same_semantics(tampered_diagnostic_rejected,
                               diagnostic_sealed) &&
                same_semantics(duplicated_seed_origin_rejected, seeded) &&
                same_semantics(invalid_seed_evidence_rejected, seeded) &&
                same_semantics(unsorted_seed_contract_rejected,
                               multi_seeded),
            "Seedentfernung oder mutierter oeffentlicher Baseline-Payload "
            "wurde als wiederverwendbar akzeptiert.");

        auto strengthened_options = seeded_options;
        strengthened_options.additional_seeds.front().guarded_candidate =
            false;
        strengthened_options.additional_seeds.front().evidence =
            katana::analysis::ControlFlowEvidence::ProvenComplete;
        strengthened_options.baseline = &seeded;
        const auto strengthened =
            katana::analysis::analyze_reachable_code(
                seed_image, strengthened_options);
        auto strengthened_fresh_options = strengthened_options;
        strengthened_fresh_options.baseline = nullptr;
        const auto strengthened_fresh =
            katana::analysis::analyze_reachable_code(
                seed_image, strengthened_fresh_options);
        auto downgraded_options = seeded_options;
        downgraded_options.baseline = &strengthened;
        const auto downgraded =
            katana::analysis::analyze_reachable_code(
                seed_image, downgraded_options);
        require(
            strengthened.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::Reused &&
                std::binary_search(
                    strengthened.proven_instruction_addresses.begin(),
                    strengthened.proven_instruction_addresses.end(),
                    4u) &&
                !std::binary_search(
                    strengthened.guarded_candidate_instruction_addresses.begin(),
                    strengthened.guarded_candidate_instruction_addresses.end(),
                    4u) &&
                downgraded.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        SeedContractMismatch &&
                downgraded.reused_contexts == 0u &&
                same_semantics(strengthened, strengthened_fresh) &&
                same_semantics(downgraded, seeded),
            "Monotone Evidenzverstaerkung fiel kalt zurueck oder ein echter "
            "Downgrade behielt stale Proven-Kontexte.");

        auto capped_strengthening_options = strengthened_fresh_options;
        capped_strengthening_options.baseline = &seeded;
        capped_strengthening_options.maximum_contexts =
            seeded.contextual_instructions.size();
        const auto capped_strengthening =
            katana::analysis::analyze_reachable_code(
                seed_image, capped_strengthening_options);
        require(
            capped_strengthening.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        EvidenceUpgradeContextRetry &&
                capped_strengthening.limit ==
                    katana::analysis::RecursiveAnalysisLimit::None &&
                capped_strengthening.reused_contexts == 0u &&
                capped_strengthening.contextual_instructions.size() <=
                    capped_strengthening_options.maximum_contexts &&
                same_semantics(
                    capped_strengthening, strengthened_fresh),
            "Alte schwache Evidenzkontexte erzeugten unter engem Cap einen "
            "falschen inkrementellen Kontextabbruch.");

        katana::analysis::RecursiveAnalysisOptions limited_options;
        limited_options.maximum_instructions = 1u;
        const auto limited =
            katana::analysis::analyze_reachable_code(
                seed_image, limited_options);
        katana::analysis::RecursiveAnalysisOptions resume_options;
        resume_options.baseline = &limited;
        const auto resumed =
            katana::analysis::analyze_reachable_code(
                seed_image, resume_options);
        require(
            limited.limit ==
                    katana::analysis::RecursiveAnalysisLimit::
                        InstructionBudgetExceeded &&
                resumed.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        IncompleteBaseline &&
                resumed.limit == katana::analysis::RecursiveAnalysisLimit::None &&
                resumed.reused_contexts == 0u &&
                resumed.instructions.size() ==
                    unseeded.instructions.size() &&
                same_semantics(resumed, unseeded) &&
                katana::analysis::format_recursive_analysis_report(limited)
                        .find("Limit: instruction-budget-exceeded") !=
                    std::string::npos &&
                katana::analysis::format_recursive_analysis_report(resumed)
                        .find("Baseline: incomplete-baseline") !=
                    std::string::npos,
            "Eine budgetlimitierte Baseline verlor ihre Frontier und wurde "
            "anschliessend als vollstaendig ausgegeben.");

        katana::analysis::RecursiveAnalysisOptions smaller_budget;
        smaller_budget.baseline = &unseeded;
        smaller_budget.maximum_instructions = 1u;
        const auto budget_rejected =
            katana::analysis::analyze_reachable_code(
                seed_image, smaller_budget);
        auto fresh_smaller_budget = smaller_budget;
        fresh_smaller_budget.baseline = nullptr;
        const auto fresh_budget_rejected =
            katana::analysis::analyze_reachable_code(
                seed_image, fresh_smaller_budget);
        require(
            budget_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        BudgetIncompatible &&
                budget_rejected.reused_contexts == 0u &&
                budget_rejected.limit ==
                    katana::analysis::RecursiveAnalysisLimit::
                        InstructionBudgetExceeded &&
                budget_rejected.instructions.size() <= 1u &&
                same_semantics(
                    budget_rejected, fresh_budget_rejected),
            "Eine Baseline oberhalb des neuen Budgets wurde vor dem Limit "
            "vollstaendig kopiert.");

        katana::analysis::RecursiveAnalysisOptions context_limited_options;
        context_limited_options.maximum_contexts = 1u;
        const auto context_limited =
            katana::analysis::analyze_reachable_code(
                seed_image, context_limited_options);
        katana::analysis::RecursiveAnalysisOptions context_resume_options;
        context_resume_options.baseline = &context_limited;
        const auto context_resumed =
            katana::analysis::analyze_reachable_code(
                seed_image, context_resume_options);
        katana::analysis::RecursiveAnalysisOptions smaller_context_budget;
        smaller_context_budget.baseline = &unseeded;
        smaller_context_budget.maximum_contexts = 1u;
        const auto context_budget_rejected =
            katana::analysis::analyze_reachable_code(
                seed_image, smaller_context_budget);
        auto fresh_smaller_context_budget = smaller_context_budget;
        fresh_smaller_context_budget.baseline = nullptr;
        const auto fresh_context_budget_rejected =
            katana::analysis::analyze_reachable_code(
                seed_image, fresh_smaller_context_budget);
        require(
            context_limited.limit ==
                    katana::analysis::RecursiveAnalysisLimit::
                        ContextBudgetExceeded &&
                context_resumed.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        IncompleteBaseline &&
                context_resumed.limit ==
                    katana::analysis::RecursiveAnalysisLimit::None &&
                context_resumed.reused_contexts == 0u &&
                same_semantics(context_resumed, unseeded) &&
                context_budget_rejected.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        BudgetIncompatible &&
                context_budget_rejected.reused_contexts == 0u &&
                context_budget_rejected.limit ==
                    katana::analysis::RecursiveAnalysisLimit::
                        ContextBudgetExceeded &&
                context_budget_rejected.contextual_instructions.size() <=
                    1u &&
                same_semantics(context_budget_rejected,
                               fresh_context_budget_rejected),
            "Ein Kontextlimit wurde ueber eine partielle oder zu grosse "
            "Baseline umgangen.");

        ExecutableImage range_image;
        range_image.add_segment(
            {".range-baseline",
             0u,
             0u,
             12u,
             SegmentKind::Code,
             {true, false, true},
             {0x0Bu, 0x00u, 0x09u, 0x00u,
              0x09u, 0x00u, 0x09u, 0x00u,
              0x0Bu, 0x00u, 0x09u, 0x00u}});
        range_image.add_entry_point(0u);
        const auto range_unseeded =
            katana::analysis::analyze_reachable_code(range_image);
        katana::analysis::RecursiveAnalysisOptions open_range_options;
        open_range_options.additional_seeds.push_back(
            {4u,
             {katana::analysis::FunctionOrigin::StoredCodeAddress},
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial});
        const auto open_range =
            katana::analysis::analyze_reachable_code(
                range_image, open_range_options);
        auto exact_range_options = open_range_options;
        exact_range_options.additional_seeds.front().function_size = 4u;
        exact_range_options.baseline = &range_unseeded;
        const auto exact_range =
            katana::analysis::analyze_reachable_code(
                range_image, exact_range_options);
        auto exact_range_fresh_options = exact_range_options;
        exact_range_fresh_options.baseline = nullptr;
        const auto exact_range_fresh =
            katana::analysis::analyze_reachable_code(
                range_image, exact_range_fresh_options);
        auto removed_range_options = open_range_options;
        removed_range_options.baseline = &exact_range_fresh;
        const auto removed_range =
            katana::analysis::analyze_reachable_code(
                range_image, removed_range_options);
        auto changed_range_options = exact_range_fresh_options;
        changed_range_options.additional_seeds.front().function_size = 8u;
        changed_range_options.baseline = &exact_range_fresh;
        const auto changed_range =
            katana::analysis::analyze_reachable_code(
                range_image, changed_range_options);
        auto changed_range_fresh_options = changed_range_options;
        changed_range_fresh_options.baseline = nullptr;
        const auto changed_range_fresh =
            katana::analysis::analyze_reachable_code(
                range_image, changed_range_fresh_options);
        const auto contains_range_address = [](const auto& analysis,
                                               const std::uint32_t address) {
            return std::any_of(
                analysis.instructions.begin(),
                analysis.instructions.end(),
                [&](const auto& line) {
                    return line.address == address;
                });
        };
        require(
            exact_range.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        SeedContractMismatch &&
                exact_range.reused_contexts == 0u &&
                contains_range_address(exact_range, 4u) &&
                contains_range_address(exact_range, 6u) &&
                !contains_range_address(exact_range, 8u) &&
                same_semantics(exact_range, exact_range_fresh) &&
                removed_range.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        SeedContractMismatch &&
                removed_range.reused_contexts == 0u &&
                contains_range_address(removed_range, 8u) &&
                same_semantics(removed_range, open_range) &&
                changed_range.baseline_status ==
                    katana::analysis::RecursiveAnalysisBaselineStatus::
                        SeedContractMismatch &&
                changed_range.reused_contexts == 0u &&
                contains_range_address(changed_range, 8u) &&
                same_semantics(changed_range, changed_range_fresh),
            "Eine hinzugefuegte, entfernte oder geaenderte exakte "
            "Funktionsgrenze verwendete alte Kontexte.");
    }
    const auto blocks = katana::analysis::build_basic_blocks(result.instructions);
    katana::analysis::InstructionArena arena(result.instructions);
    const auto spans = katana::analysis::build_block_spans(arena, blocks);
    const std::vector<katana::analysis::ResolvedControlFlowEdge> no_edges;
    const std::vector<katana::analysis::FunctionInfo> no_functions;
    const katana::analysis::AnalysisIndex index(
        arena, blocks, no_edges, no_functions, image.segments());
    katana::analysis::EvidenceInterner evidence;
    const auto first_evidence = evidence.intern("entry-point");
    require(!spans.empty() && spans.front().view(arena).front().address == 0x8C010000u &&
                index.instruction(0x8C010000u).has_value() &&
                index.block(blocks.front().start_address).has_value() &&
                index.segment(0x8C020000u).has_value() &&
                evidence.intern("entry-point") == first_evidence &&
                evidence.resolve(first_evidence) == "entry-point",
            "Instruktionsarena, Blockspan oder gemeinsamer Analyseindex ist inkonsistent.");
    const auto guarded_function =
        std::find_if(guarded_result.functions.begin(),
                     guarded_result.functions.end(),
                     [](const auto& function) { return function.address == 0x8C01000Cu; });
    require(guarded_function != guarded_result.functions.end() &&
                guarded_function->confidence == katana::analysis::AnalysisConfidence::Medium &&
                guarded_function->origins ==
                    std::vector{katana::analysis::FunctionOrigin::GuardedSnapshot} &&
                std::string(katana::analysis::function_origin_name(
                    katana::analysis::FunctionOrigin::GuardedSnapshot)) == "guarded-snapshot",
            "Bewachte Snapshotfunktion verlor Herkunft oder mittlere Konfidenz.");

    ExecutableImage invalid;
    invalid.add_segment(
        {".data", 0x1000u, 0u, 2u, SegmentKind::Data, {true, true, false}, {0u, 0u}});
    invalid.add_entry_point(0x1000u);
    bool rejected = false;
    try {
        static_cast<void>(katana::analysis::analyze_reachable_code(invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Ein Einstiegspunkt in Daten wurde akzeptiert.");

    ExecutableImage aliased_entry;
    aliased_entry.set_address_model(ImageAddressModel::Sh4DirectMapped);
    aliased_entry.add_segment(
        {".text", 0x8C001000u, 0u, 2u, SegmentKind::Code, {true, false, true}, {0x0Bu, 0x00u}});
    aliased_entry.add_entry_point(0xAC001000u);
    const auto aliased_result = katana::analysis::analyze_reachable_code(aliased_entry);
    require(aliased_result.instructions.size() == 1u &&
                aliased_result.instructions[0].address == 0x8C001000u &&
                aliased_result.functions.size() == 1u &&
                aliased_result.functions[0].address == 0x8C001000u,
            "Ein P2-Einstiegspunkt wurde nicht auf die kanonische P1-Codeadresse normalisiert.");

    ExecutableImage overlap;
    overlap.add_segment(
        {".text",
         0x1000u,
         0u,
         12u,
         SegmentKind::Code,
         {true, false, true},
         {0x02u, 0xB0u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    overlap.add_entry_point(0x1002u);
    overlap.add_symbol({"owner", 0x1000u, 4u, SymbolKind::Function, SymbolBinding::Global});
    const auto overlap_result = katana::analysis::analyze_reachable_code(overlap);
    require(overlap_result.conflicts.size() == 1u &&
                overlap_result.conflicts[0].address == 0x1002u &&
                overlap_result.conflicts[0].size == 2u &&
                overlap_result.conflicts[0].kind ==
                    katana::analysis::AnalysisConflictKind::FunctionEntryInDelaySlot,
            "Ueberlappende Funktions- und Delay-Slot-Rollen wurden nicht gemeldet.");
    require(std::string(katana::analysis::analysis_conflict_kind_name(
                overlap_result.conflicts[0].kind)) == "function-entry-in-delay-slot",
            "Konfliktname ist instabil.");
    const auto normal_context = std::count_if(overlap_result.contextual_instructions.begin(),
                                              overlap_result.contextual_instructions.end(),
                                              [](const auto& contextual) {
                                                  return contextual.line.address == 0x1002u &&
                                                         !contextual.delay_slot_owner.has_value();
                                              });
    const auto slot_context = std::count_if(overlap_result.contextual_instructions.begin(),
                                            overlap_result.contextual_instructions.end(),
                                            [](const auto& contextual) {
                                                return contextual.line.address == 0x1002u &&
                                                       contextual.delay_slot_owner == 0x1000u;
                                            });
    require(
        normal_context == 1 && slot_context == 1,
        "Dieselbe Adresse wurde nicht getrennt als normale Instruktion und Delay Slot erfasst.");
    const auto report = katana::analysis::format_recursive_analysis_report(result);
    require(report == katana::analysis::format_recursive_analysis_report(result),
            "Analysebericht ist nicht deterministisch.");
    require(report.find("Funktion 0x8C010000 Konfidenz=certain Evidenz=proven-complete "
                        "Herkunft=entry-point") != std::string::npos,
            "Analysebericht erklaert den Einstiegspunkt nicht.");
    require(report.find("Funktion 0x8C010008 Konfidenz=high Evidenz=proven-complete "
                        "Herkunft=direct-call,symbol") != std::string::npos,
            "Analysebericht erklaert den Call-/Symbolkandidaten nicht.");
    require(report.find("Unerreichbar 0x8C01000C Groesse=2") != std::string::npos,
            "Unerreichbarer Bereich fehlt im Bericht.");

    ExecutableImage unknown;
    unknown.add_segment({".text",
                         0u,
                         0u,
                         6u,
                         SegmentKind::Code,
                         {true, false, true},
                         {0x09u, 0x00u, 0xFFu, 0xFFu, 0x09u, 0x00u}});
    unknown.add_entry_point(0u);
    const auto unknown_result = katana::analysis::analyze_reachable_code(unknown);
    require(unknown_result.instructions.size() == 2u &&
                unknown_result.instructions.back().address == 2u,
            "Unbekannter Opcode hat den linearen Analysepfad nicht beendet.");
    require(unknown_result.diagnostics.size() == 1u &&
                unknown_result.diagnostics[0].address == 2u &&
                unknown_result.diagnostics[0].opcode == 0xFFFFu &&
                unknown_result.diagnostics[0].kind ==
                    katana::analysis::AnalysisDiagnosticKind::UnknownOpcode &&
                unknown_result.diagnostics[0].reason == "unknown-opcode" &&
                unknown_result.diagnostics[0].evidence ==
                    katana::analysis::ControlFlowEvidence::ProvenComplete &&
                katana::analysis::analysis_diagnostic_blocks_codegen(
                    unknown_result.diagnostics[0]),
            "Unbekannter Opcode wurde nicht stabil diagnostiziert.");
    const auto unknown_report = katana::analysis::format_recursive_analysis_report(unknown_result);
    require(unknown_report.find("Diagnose 0x00000002 Opcode=0xFFFF Grund=unknown-opcode") !=
                std::string::npos,
            "Bericht nennt Adresse, Opcode und Abbruchgrund nicht.");

    ExecutableImage guarded_unknown;
    guarded_unknown.add_segment(
        {".mixed",
         0u,
         0u,
         8u,
         SegmentKind::Mixed,
         {true, true, true},
         {0x0Bu, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0xFFu, 0xFFu}});
    guarded_unknown.add_entry_point(0u);
    katana::analysis::RecursiveAnalysisOptions guarded_unknown_options;
    guarded_unknown_options.additional_seeds.push_back(
        {4u,
         {katana::analysis::FunctionOrigin::StoredCodeAddress},
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial});
    const auto guarded_unknown_result =
        katana::analysis::analyze_reachable_code(guarded_unknown, guarded_unknown_options);
    const auto guarded_unknown_diagnostic =
        std::find_if(guarded_unknown_result.diagnostics.begin(),
                     guarded_unknown_result.diagnostics.end(),
                     [](const auto& diagnostic) { return diagnostic.address == 6u; });
    require(guarded_unknown_diagnostic != guarded_unknown_result.diagnostics.end() &&
                guarded_unknown_diagnostic->reason == "unknown-opcode" &&
                guarded_unknown_diagnostic->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedPartial &&
                !katana::analysis::analysis_diagnostic_blocks_codegen(
                    *guarded_unknown_diagnostic),
            "Bewachter Stored-Code-Kandidat verlor seine nicht blockierende Diagnoseevidenz.");

    ExecutableImage upgraded_unknown;
    upgraded_unknown.add_segment(
        {".mixed",
         0u,
         0u,
         2u,
         SegmentKind::Mixed,
         {true, true, true},
         {0xFFu, 0xFFu}});
    katana::analysis::RecursiveAnalysisOptions candidate_only_options;
    candidate_only_options.additional_seeds.push_back(
        {0u,
         {katana::analysis::FunctionOrigin::StoredCodeAddress},
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial});
    const auto candidate_only =
        katana::analysis::analyze_reachable_code(upgraded_unknown, candidate_only_options);
    upgraded_unknown.add_entry_point(0u);
    auto upgraded_options = candidate_only_options;
    upgraded_options.baseline = &candidate_only;
    const auto upgraded_result =
        katana::analysis::analyze_reachable_code(upgraded_unknown, upgraded_options);
    require(upgraded_result.diagnostics.size() == 1u &&
                upgraded_result.diagnostics.front().evidence ==
                    katana::analysis::ControlFlowEvidence::ProvenComplete &&
                katana::analysis::analysis_diagnostic_blocks_codegen(
                    upgraded_result.diagnostics.front()),
            "Staerkere spaetere Diagnoseevidenz ging beim Baseline-Merge verloren.");

    ExecutableImage forced_unknown;
    forced_unknown.add_segment(
        {".mixed",
         0u,
         0u,
         2u,
         SegmentKind::Mixed,
         {true, true, true},
         {0xFFu, 0xFFu}});
    katana::analysis::RecursiveAnalysisOptions forced_unknown_options;
    forced_unknown_options.additional_seeds.push_back(
        {0u,
         {katana::analysis::FunctionOrigin::StoredCodeAddress},
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial});
    forced_unknown_options.additional_seeds.push_back(
        {0u,
         {katana::analysis::FunctionOrigin::UserOverride},
         false,
         katana::analysis::ControlFlowEvidence::ForcedOverride});
    const auto forced_unknown_result =
        katana::analysis::analyze_reachable_code(forced_unknown, forced_unknown_options);
    require(forced_unknown_result.diagnostics.size() == 1u &&
                forced_unknown_result.diagnostics.front().evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride &&
                katana::analysis::analysis_diagnostic_blocks_codegen(
                    forced_unknown_result.diagnostics.front()),
            "Expliziter Override-Blocker verlor gegen staerkere Kandidatenevidenz.");

    ExecutableImage alternate;
    alternate.add_segment({".text",
                           0u,
                           0u,
                           10u,
                           SegmentKind::Code,
                           {true, false, true},
                           {0x09u, 0x00u, 0xFFu, 0xFFu, 0x0Bu, 0x00u, 0xFDu, 0xAFu, 0x09u, 0x00u}});
    alternate.add_entry_point(0u);
    alternate.add_entry_point(6u);
    const auto alternate_result = katana::analysis::analyze_reachable_code(alternate);
    require(std::any_of(alternate_result.instructions.begin(),
                        alternate_result.instructions.end(),
                        [](const auto& line) { return line.address == 4u; }),
            "Ein separates direktes Sprungziel hinter einem unbekannten Opcode wurde nicht "
            "analysiert.");

    ExecutableImage unknown_delay;
    unknown_delay.add_segment(
        {".text",
         0u,
         0u,
         12u,
         SegmentKind::Code,
         {true, false, true},
         {0x02u, 0xB0u, 0xFFu, 0xFFu, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    unknown_delay.add_entry_point(0u);
    const auto unknown_delay_result = katana::analysis::analyze_reachable_code(unknown_delay);
    require(unknown_delay_result.instructions.size() == 2u &&
                unknown_delay_result.instructions[1].is_delay_slot &&
                unknown_delay_result.diagnostics.size() == 2u &&
                std::any_of(
                    unknown_delay_result.diagnostics.begin(),
                    unknown_delay_result.diagnostics.end(),
                    [](const auto& diagnostic) { return diagnostic.reason == "unknown-opcode"; }) &&
                std::any_of(unknown_delay_result.diagnostics.begin(),
                            unknown_delay_result.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.reason == "delay-slot-unknown-opcode";
                            }),
            "Unbekannter Delay Slot wurde nicht vollstaendig diagnostiziert.");
    require(std::none_of(unknown_delay_result.instructions.begin(),
                         unknown_delay_result.instructions.end(),
                         [](const auto& line) { return line.address == 4u || line.address == 8u; }),
            "Unbekannter Delay Slot liess unsicheren Call- oder Rueckkehrpfad weiterlaufen.");

    ExecutableImage control_in_delay;
    control_in_delay.add_segment({".text",
                                  0u,
                                  0u,
                                  8u,
                                  SegmentKind::Code,
                                  {true, false, true},
                                  {0x00u, 0xB0u, 0xFDu, 0xAFu, 0x09u, 0x00u, 0x09u, 0x00u}});
    control_in_delay.add_entry_point(0u);
    const auto control_in_delay_result = katana::analysis::analyze_reachable_code(control_in_delay);
    require(std::any_of(control_in_delay_result.diagnostics.begin(),
                        control_in_delay_result.diagnostics.end(),
                        [](const auto& diagnostic) {
                            return diagnostic.address == 2u &&
                                   diagnostic.kind ==
                                       katana::analysis::AnalysisDiagnosticKind::
                                           ControlFlowInDelaySlot &&
                                   diagnostic.reason == "control-flow-in-delay-slot";
                        }),
            "Kontrollfluss im Delay Slot wurde nicht kontexttreu diagnostiziert.");
    ExecutableImage guarded_control_in_delay;
    guarded_control_in_delay.add_segment(
        {".mixed",
         0u,
         0u,
         4u,
         SegmentKind::Mixed,
         {true, true, true},
         {0x00u, 0xB0u, 0xFDu, 0xAFu}});
    katana::analysis::RecursiveAnalysisOptions guarded_control_in_delay_options;
    guarded_control_in_delay_options.additional_seeds.push_back(
        {0u,
         {katana::analysis::FunctionOrigin::StoredCodeAddress},
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial});
    const auto guarded_control_in_delay_result = katana::analysis::analyze_reachable_code(
        guarded_control_in_delay, guarded_control_in_delay_options);
    require(std::any_of(
                guarded_control_in_delay_result.diagnostics.begin(),
                guarded_control_in_delay_result.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.kind ==
                               katana::analysis::AnalysisDiagnosticKind::
                                   ControlFlowInDelaySlot &&
                           diagnostic.evidence ==
                               katana::analysis::ControlFlowEvidence::GuardedPartial &&
                           katana::analysis::analysis_diagnostic_blocks_codegen(diagnostic);
                }),
            "Struktureller Kontrollflussfehler wurde durch Kandidatenevidenz entschaerft.");

    ExecutableImage missing_delay;
    missing_delay.add_segment(
        {".text", 0u, 0u, 2u, SegmentKind::Code, {true, false, true}, {0x00u, 0xB0u}});
    missing_delay.add_entry_point(0u);
    const auto missing_delay_result = katana::analysis::analyze_reachable_code(missing_delay);
    require(missing_delay_result.diagnostics.size() == 1u &&
                missing_delay_result.diagnostics.front().address == 0u &&
                missing_delay_result.diagnostics.front().kind ==
                    katana::analysis::AnalysisDiagnosticKind::DelaySlotUnavailable &&
                missing_delay_result.diagnostics.front().reason == "delay-slot-unavailable",
            "Fehlender Delay Slot wurde nicht am Owner sichtbar diagnostiziert.");
    ExecutableImage guarded_missing_delay;
    guarded_missing_delay.add_segment(
        {".mixed", 0u, 0u, 2u, SegmentKind::Mixed, {true, true, true}, {0x00u, 0xB0u}});
    katana::analysis::RecursiveAnalysisOptions guarded_missing_delay_options;
    guarded_missing_delay_options.additional_seeds.push_back(
        {0u,
         {katana::analysis::FunctionOrigin::StoredCodeAddress},
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial});
    const auto guarded_missing_delay_result = katana::analysis::analyze_reachable_code(
        guarded_missing_delay, guarded_missing_delay_options);
    require(guarded_missing_delay_result.diagnostics.size() == 1u &&
                guarded_missing_delay_result.diagnostics.front().evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedPartial &&
                katana::analysis::analysis_diagnostic_blocks_codegen(
                    guarded_missing_delay_result.diagnostics.front()),
            "Fehlender Candidate-Delay-Slot wurde faelschlich als nicht bindend behandelt.");

    ExecutableImage prefetch;
    prefetch.add_segment({".text",
                          0u,
                          0u,
                          8u,
                          SegmentKind::Code,
                          {true, false, true},
                          {0x83u, 0x03u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    prefetch.add_entry_point(0u);
    const auto prefetch_result = katana::analysis::analyze_reachable_code(prefetch);
    require(prefetch_result.instructions.size() == 4u &&
                prefetch_result.instructions[0].instruction.kind ==
                    katana::sh4::InstructionKind::Prefetch &&
                prefetch_result.instructions.back().address == 6u &&
                prefetch_result.diagnostics.empty(),
            "PREF beendet die rekursive Analyse faelschlich als unbekannter Opcode.");

    std::cout << "KR-1701 Worklist ab Einstiegspunkten erfolgreich.\n";
    return EXIT_SUCCESS;
}
