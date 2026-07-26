#include "katana/sh4/isa_coverage.hpp"

#include "katana/build_contract.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/io/json_report.hpp"
#include "katana/sh4/external_evidence_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct EvidenceFixture {
    std::string katana_commit{katana::build_contract::katana_git_commit};
    std::string corpus_commit{katana::sh4::external_evidence_contract::corpus_commit};
    std::string corpus_hash{katana::sh4::external_evidence_contract::corpus_manifest_sha256};
    std::uint32_t runtime_abi = katana::build_contract::runtime_abi_version;
    std::uint32_t backend_abi = katana::build_contract::backend_interface_abi_version;
    std::string backend_profile{"external-conformance"};
    std::uint32_t backend_profile_version = katana::codegen::native_aot_emission_profile_version;
    std::string scope{"smoke"};
    bool complete_scope = true;
    std::uint64_t expected_scope_vectors =
        katana::sh4::external_evidence_contract::smoke_vector_count;
    std::optional<std::string> file;
    std::optional<std::uint64_t> case_index;
    std::optional<std::uint64_t> opcode;
    std::optional<std::string> family;
    std::optional<std::uint64_t> shard_index;
    std::optional<std::uint64_t> shard_count;
    bool fail_fast = false;
    std::uint64_t total = katana::sh4::external_evidence_contract::smoke_vector_count;
    std::uint64_t applicable = katana::sh4::external_evidence_contract::smoke_vector_count - 1u;
    std::uint64_t passed = katana::sh4::external_evidence_contract::smoke_vector_count - 1u;
    std::uint64_t failed = 0u;
};

std::string evidence_document(const EvidenceFixture& fixture) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-sh4-sst-conformance\",\"report_version\":1,"
              "\"report_type\":\"sh4-sst-conformance\",\"status\":"
           << katana::io::quote_json(fixture.failed == 0u ? "success" : "failure")
           << ",\"katana_commit\":" << katana::io::quote_json(fixture.katana_commit)
           << ",\"corpus_commit\":" << katana::io::quote_json(fixture.corpus_commit)
           << ",\"corpus_manifest_sha256\":" << katana::io::quote_json(fixture.corpus_hash)
           << ",\"compiler\":\"synthetic compiler C:\\\\private\\\\tool\","
              "\"build_type\":\"Release\",\"host_platform\":\"synthetic\",\"lto\":true,"
              "\"runtime_abi\":"
           << fixture.runtime_abi << ",\"backend_abi\":" << fixture.backend_abi
           << ",\"backend_profile\":" << katana::io::quote_json(fixture.backend_profile)
           << ",\"backend_profile_version\":" << fixture.backend_profile_version
           << ",\"scope\":" << katana::io::quote_json(fixture.scope)
           << ",\"selection\":{\"complete_scope\":" << (fixture.complete_scope ? "true" : "false")
           << ",\"expected_scope_vectors\":" << fixture.expected_scope_vectors << ",\"file\":";
    if (fixture.file)
        output << katana::io::quote_json(*fixture.file);
    else
        output << "null";
    output << ",\"case\":";
    if (fixture.case_index)
        output << *fixture.case_index;
    else
        output << "null";
    output << ",\"opcode\":";
    if (fixture.opcode)
        output << *fixture.opcode;
    else
        output << "null";
    output << ",\"family\":";
    if (fixture.family)
        output << katana::io::quote_json(*fixture.family);
    else
        output << "null";
    output << ",\"shard\":";
    if (fixture.shard_index && fixture.shard_count)
        output << "{\"index\":" << *fixture.shard_index << ",\"count\":" << *fixture.shard_count
               << '}';
    else
        output << "null";
    output << ",\"fail_fast\":" << (fixture.fail_fast ? "true" : "false")
           << "},\"memory_profile\":\"native-product-memory\","
              "\"fpu_comparison_mode\":\"strict\",\"counts\":{\"total\":"
           << fixture.total << ",\"applicable\":" << fixture.applicable
           << ",\"passed\":" << fixture.passed << ",\"failed\":" << fixture.failed
           << "},\"classification_counts\":{\"pass\":" << fixture.passed
           << ",\"fail-state\":" << fixture.failed
           << ",\"fail-control-flow\":0,\"fail-delay-slot\":0,"
              "\"fail-memory-address\":0,\"fail-memory-width\":0,"
              "\"fail-memory-value\":0,\"fail-memory-order\":0,"
              "\"fail-extra-side-effect\":0,\"fail-unbound-target\":0,"
              "\"fail-unexpected-exception\":0,"
              "\"not-applicable-reference-alignment\":0,"
              "\"not-applicable-reference-exception\":0,"
              "\"not-applicable-reference-mmio\":0,"
              "\"not-applicable-reference-known-bug\":0,"
              "\"not-applicable-katana-restricted\":"
           << fixture.total - fixture.applicable
           << ",\"not-applicable-access-shape\":0,\"corpus-invalid\":0,"
              "\"harness-invalid\":0,\"infrastructure-error\":0},"
              "\"used_files\":[\"C:\\\\private\\\\corpus\\\\case.json.bin\"],"
              "\"represented_opcodes\":[],"
              "\"katana_opcodes_without_external_evidence\":[],"
              "\"waivers\":[],\"first_counterexamples\":[]}";
    return output.str();
}

std::string replace_evidence_fragment(std::string document,
                                      const std::string_view old_fragment,
                                      const std::string_view new_fragment) {
    const auto position = document.find(old_fragment);
    if (position == std::string::npos) throw std::runtime_error("SST-Evidence-Testfragment fehlt.");
    document.replace(position, old_fragment.size(), new_fragment);
    return document;
}

bool rejects_external_evidence(const std::string& document) {
    try {
        static_cast<void>(katana::sh4::parse_external_isa_evidence_json(document));
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

} // namespace

int main() {
    using namespace katana::sh4;
    constexpr std::string_view untrusted_source_commit = "0000000000000000000000000000000000000000";
    const bool trusted_source =
        katana::build_contract::katana_git_commit != untrusted_source_commit;
    static_assert(external_evidence_contract::smoke_vector_count == 65u);
    static_assert(external_evidence_contract::full_vector_count == 116500u);
    const auto report = build_isa_coverage_report();

    require(report.instructions.size() == 163u,
            "Nicht jede implementierte Instruktionsart erscheint im Bericht.");
    require(report.known_opcode_count + report.unknown_opcode_count == 65536u,
            "Der Bericht deckt den 16-Bit-Opcode-Raum nicht vollstaendig ab.");
    require(report.known_opcode_count != 0u, "Der Bericht zaehlt keine bekannten Opcodes.");
    require(report.unknown_opcode_count != 0u, "Der Bericht verschweigt unbekannte Opcodes.");
    require(report.families.size() == 7u,
            "Der Alpha-ISA-Vertrag besitzt nicht alle benannten Familien.");

    const auto family = [&](const std::string& id) -> const AlphaIsaFamilyEntry& {
        const auto found = std::find_if(report.families.begin(),
                                        report.families.end(),
                                        [&](const auto& entry) { return entry.id == id; });
        require(found != report.families.end(), "Alpha-ISA-Familie fehlt: " + id);
        return *found;
    };
    require(family("integer-core").support == AlphaIsaSupport::Supported &&
                family("system-control").support == AlphaIsaSupport::Restricted &&
                family("system-control").layers.runtime == AlphaIsaSupport::Restricted &&
                family("fpu").support == AlphaIsaSupport::Restricted &&
                family("unknown-opcode").support == AlphaIsaSupport::Rejected,
            "Alpha-ISA-Familien verschweigen Unterstuetzung, Einschraenkung oder Ablehnung.");
    require(std::all_of(report.families.begin(),
                        report.families.end(),
                        [](const auto& entry) {
                            return !entry.id.empty() && !entry.name.empty() &&
                                   !entry.semantic_contract.empty() &&
                                   !entry.test_requirement.empty() &&
                                   (entry.support == AlphaIsaSupport::Supported ||
                                    !entry.limitation.empty()) &&
                                   (entry.support != AlphaIsaSupport::Supported ||
                                    (entry.layers.decoder == AlphaIsaSupport::Supported &&
                                     entry.layers.ir == AlphaIsaSupport::Supported &&
                                     entry.layers.backend == AlphaIsaSupport::Supported &&
                                     entry.layers.runtime == AlphaIsaSupport::Supported));
                        }),
            "Alpha-ISA-Behauptung besitzt keine Semantik, Grenze oder Testanforderung.");

    const auto missing = std::find_if(
        report.instructions.begin(), report.instructions.end(), [](const IsaCoverageEntry& entry) {
            return entry.encoding_rule_count == 0u || entry.decoded_opcode_count == 0u ||
                   entry.name.empty();
        });
    require(missing == report.instructions.end(),
            "Eine implementierte Instruktion ist ohne Regel, Namen oder Opcode sichtbar.");
    require(std::all_of(report.instructions.begin(),
                        report.instructions.end(),
                        [&](const auto& entry) {
                            const auto& contract = family(entry.family_id);
                            return entry.support == contract.support &&
                                   entry.test_requirement == contract.test_requirement;
                        }),
            "Instruktionsart widerspricht ihrem gemeinsamen Alpha-Familienvertrag.");
    require(alpha_isa_intersection({AlphaIsaSupport::Supported,
                                    AlphaIsaSupport::Supported,
                                    AlphaIsaSupport::Rejected,
                                    AlphaIsaSupport::Supported}) == AlphaIsaSupport::Rejected &&
                std::all_of(report.instructions.begin(),
                            report.instructions.end(),
                            [](const auto& entry) {
                                return entry.support == alpha_isa_intersection(entry.layers);
                            }),
            "Ein fehlender Backendlayer wird nicht unabhaengig als rejected gemeldet.");

    const auto text = format_isa_coverage_report(report);
    require(text.find("ReturnFromException") != std::string::npos,
            "RTE fehlt im formatierten Bericht.");
    require(text.find("StoreSpecialRegister") != std::string::npos,
            "Systemregistertransfers fehlen im formatierten Bericht.");
    require(text.find("FcnvSingleToDouble") != std::string::npos,
            "FPU-Instruktionen fehlen im formatierten Bericht.");
    require(text.find("LoadTlb") != std::string::npos && text.find("Ocbi") != std::string::npos &&
                text.find("MovcaLong") != std::string::npos,
            "Der ISA-Bericht verschweigt die geschlossenen SH7750-Instruktionsluecken.");
    require(text.find("Unbekannte Opcodes:") != std::string::npos,
            "Die Unknown-Abdeckung fehlt im Bericht.");
    require(text.find("restricted") != std::string::npos &&
                text.find("SLEEP wakeup") != std::string::npos &&
                text.find("user-mode privilege violations trap") != std::string::npos &&
                text.find("Privilege violations and complete") == std::string::npos,
            "Textbericht verschweigt Alpha-Einschraenkungen oder ihre Semantik.");

    const auto json = format_alpha_isa_json(report);
    require(json.find("\"schema\":\"katana-alpha-isa\"") != std::string::npos &&
                json.find("\"contract_version\":1") != std::string::npos &&
                json.find("\"runtime\":\"restricted\"") != std::string::npos &&
                json.find("\"backend\":\"supported\"") != std::string::npos &&
                json.find("\"id\":\"unknown-opcode\"") != std::string::npos &&
                json.find("\"status\":\"rejected\"") != std::string::npos &&
                json.find("\"test_requirement\"") != std::string::npos &&
                json.find("\"external_evidence\":null") != std::string::npos,
            "Maschinenlesbarer Alpha-ISA-Bericht verliert Schichten, Grenzen oder Tests.");

    require(katana::build_contract::katana_git_commit.size() == 40u,
            "Buildvertrag enthaelt keinen exakten Katana-Git-Commit.");
    const auto evidence = parse_external_isa_evidence_json(evidence_document(EvidenceFixture{}));
    require(evidence.stale == !trusted_source &&
                evidence.stale_reasons ==
                    (trusted_source ? std::vector<std::string>{}
                                    : std::vector<std::string>{"untrusted-build-source"}) &&
                evidence.source == "SingleStepTests/sh4" && evidence.complete_scope &&
                evidence.expected_scope_vectors == 65u && evidence.counts.total == 65u &&
                evidence.counts.applicable == 64u && evidence.counts.passed == 64u &&
                evidence.counts.failed == 0u && evidence.counts.not_applicable == 1u &&
                evidence.waiver_count == 0u,
            "Aktuelle externe SST-Evidence wird nicht korrekt uebernommen.");

    const auto evidence_json = format_alpha_isa_json(report, evidence);
    require(evidence_json.find("\"external_evidence\":{\"evidence_version\":1") !=
                    std::string::npos &&
                evidence_json.find("\"source\":\"SingleStepTests/sh4\"") != std::string::npos &&
                evidence_json.find("\"complete_scope\":true") != std::string::npos &&
                evidence_json.find("\"expected_scope_vectors\":65") != std::string::npos &&
                evidence_json.find("\"counts\":{\"total\":65,\"applicable\":64,"
                                   "\"passed\":64,\"failed\":0,\"not_applicable\":1}") !=
                    std::string::npos &&
                evidence_json.find("\"fpu_comparison_mode\":\"strict\"") != std::string::npos &&
                evidence_json.find("\"waivers\":0") != std::string::npos &&
                evidence_json.find(trusted_source ? "\"stale\":false" : "\"stale\":true") !=
                    std::string::npos &&
                evidence_json.find("C:\\\\private") == std::string::npos,
            "ISA-Evidence verliert Zaehler oder gibt lokale Quellpfade weiter.");

    EvidenceFixture stale_fixture;
    stale_fixture.katana_commit = "0000000000000000000000000000000000000000";
    stale_fixture.corpus_commit = "1111111111111111111111111111111111111111";
    stale_fixture.corpus_hash = "2222222222222222222222222222222222222222222222222222222222222222";
    ++stale_fixture.runtime_abi;
    ++stale_fixture.backend_abi;
    stale_fixture.backend_profile = "product";
    ++stale_fixture.backend_profile_version;
    const auto stale = parse_external_isa_evidence_json(evidence_document(stale_fixture));
    require(stale.stale && stale.stale_reasons.size() == (trusted_source ? 8u : 7u) &&
                std::find(stale.stale_reasons.begin(),
                          stale.stale_reasons.end(),
                          "untrusted-build-source") != stale.stale_reasons.end(),
            "Veraltete Commit-, Corpus-, ABI- oder Backend-Evidence wird nicht stale.");

    EvidenceFixture incomplete_fixture;
    incomplete_fixture.complete_scope = false;
    incomplete_fixture.file = "case.json.bin";
    incomplete_fixture.total = 1u;
    incomplete_fixture.applicable = 1u;
    incomplete_fixture.passed = 1u;
    const auto incomplete = parse_external_isa_evidence_json(evidence_document(incomplete_fixture));
    const auto expected_incomplete_reasons =
        trusted_source ? std::vector<std::string>{"incomplete-scope"}
                       : std::vector<std::string>{"untrusted-build-source", "incomplete-scope"};
    require(incomplete.stale && !incomplete.complete_scope &&
                incomplete.expected_scope_vectors == 65u &&
                incomplete.stale_reasons == expected_incomplete_reasons,
            "Gefilterte oder gekuerzte SST-Evidence wird nicht als unvollstaendig markiert.");
    const auto incomplete_json = format_alpha_isa_json(report, incomplete);
    require(incomplete_json.find("\"complete_scope\":false") != std::string::npos &&
                incomplete_json.find("\"expected_scope_vectors\":65") != std::string::npos &&
                incomplete_json.find(trusted_source
                                         ? "\"stale_reasons\":[\"incomplete-scope\"]"
                                         : "\"stale_reasons\":[\"untrusted-build-source\","
                                           "\"incomplete-scope\"]") != std::string::npos,
            "Unvollstaendige SST-Evidence verliert Vollstaendigkeitsmetadaten.");

    EvidenceFixture full_fixture;
    full_fixture.scope = "full";
    full_fixture.expected_scope_vectors = external_evidence_contract::full_vector_count;
    full_fixture.total = external_evidence_contract::full_vector_count;
    full_fixture.applicable = external_evidence_contract::full_vector_count;
    full_fixture.passed = external_evidence_contract::full_vector_count;
    const auto full = parse_external_isa_evidence_json(evidence_document(full_fixture));
    require(full.stale == !trusted_source && full.complete_scope &&
                full.expected_scope_vectors == external_evidence_contract::full_vector_count,
            "Vollstaendige Full-SST-Evidence wird nicht als frisch anerkannt.");

    EvidenceFixture invalid_scope;
    invalid_scope.scope = "diagnostic";
    EvidenceFixture invalid_denominator;
    ++invalid_denominator.expected_scope_vectors;
    EvidenceFixture filtered_complete;
    filtered_complete.file = "case.json.bin";
    EvidenceFixture invalid_shard;
    invalid_shard.complete_scope = false;
    invalid_shard.shard_index = 2u;
    invalid_shard.shard_count = 2u;
    EvidenceFixture oversized_scope;
    oversized_scope.complete_scope = false;
    ++oversized_scope.total;
    ++oversized_scope.applicable;
    ++oversized_scope.passed;
    const auto invalid_selection_type = replace_evidence_fragment(
        evidence_document(EvidenceFixture{}), "\"complete_scope\":true", "\"complete_scope\":1");
    const auto unknown_classification = replace_evidence_fragment(
        evidence_document(EvidenceFixture{}), "\"harness-invalid\":0", "\"unknown-invalid\":0");
    const auto extra_classification =
        replace_evidence_fragment(evidence_document(EvidenceFixture{}),
                                  "\"infrastructure-error\":0}",
                                  "\"infrastructure-error\":0,\"unexpected\":0}");
    const auto non_integer_classification =
        replace_evidence_fragment(evidence_document(EvidenceFixture{}),
                                  "\"fail-control-flow\":0",
                                  "\"fail-control-flow\":false");
    const auto incomplete_classification_sum =
        replace_evidence_fragment(evidence_document(EvidenceFixture{}),
                                  "\"not-applicable-katana-restricted\":1",
                                  "\"not-applicable-katana-restricted\":0");
    const auto invalid_classification = replace_evidence_fragment(
        evidence_document(EvidenceFixture{}), "\"corpus-invalid\":0", "\"corpus-invalid\":1");

    auto incompatible = evidence_document(EvidenceFixture{});
    const auto schema = incompatible.find("katana-sh4-sst-conformance");
    incompatible.replace(schema, std::string("katana-sh4-sst-conformance").size(), "other");
    require(rejects_external_evidence(evidence_document(invalid_scope)) &&
                rejects_external_evidence(evidence_document(invalid_denominator)) &&
                rejects_external_evidence(evidence_document(filtered_complete)) &&
                rejects_external_evidence(evidence_document(invalid_shard)) &&
                rejects_external_evidence(evidence_document(oversized_scope)) &&
                rejects_external_evidence(invalid_selection_type) &&
                rejects_external_evidence(unknown_classification) &&
                rejects_external_evidence(extra_classification) &&
                rejects_external_evidence(non_integer_classification) &&
                rejects_external_evidence(incomplete_classification_sum) &&
                rejects_external_evidence(invalid_classification) &&
                rejects_external_evidence(incompatible) &&
                rejects_external_evidence("{\"schema\":") &&
                rejects_external_evidence("{\"schema\":\"katana-sh4-sst-conformance\","
                                          "\"schema\":\"katana-sh4-sst-conformance\"}"),
            "Malformed, inkompatible, gefiltert-vollstaendige, falsch klassifizierte oder "
            "mehrdeutige SST-Evidence wird akzeptiert.");

    std::cout << "KR-1503/KR-4501 messbarer Alpha-ISA-Vertrag erfolgreich.\n";
    return EXIT_SUCCESS;
}
