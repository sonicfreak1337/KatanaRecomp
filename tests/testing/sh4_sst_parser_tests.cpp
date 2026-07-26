#include "katana/io/input_provenance.hpp"
#include "katana/testing/sh4_sst.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using katana::testing::FpuComparisonMode;
using katana::testing::ResultClassification;
using katana::testing::SstCorpusInvalid;
using katana::testing::SstParserOptions;
using katana::testing::SstState;

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

void put_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8u)));
}

void put_u64(std::vector<std::uint8_t>& bytes, const std::uint64_t value) {
    for (std::size_t index = 0u; index < 8u; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8u)));
}

void overwrite_u32(std::vector<std::uint8_t>& bytes,
                   const std::size_t offset,
                   const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (index * 8u));
}

SstState make_state(const std::uint32_t seed, const std::uint32_t sr, const std::uint32_t fpscr) {
    SstState state;
    for (std::size_t index = 0u; index < state.r.size(); ++index)
        state.r[index] = seed + static_cast<std::uint32_t>(index);
    for (std::size_t index = 0u; index < state.r_bank.size(); ++index)
        state.r_bank[index] = seed + 0x100u + static_cast<std::uint32_t>(index);
    for (std::size_t index = 0u; index < state.fp0.size(); ++index) {
        state.fp0[index] = seed + 0x200u + static_cast<std::uint32_t>(index);
        state.fp1[index] = seed + 0x300u + static_cast<std::uint32_t>(index);
    }
    state.pc = seed + 0x400u;
    state.gbr = seed + 0x401u;
    state.sr = sr;
    state.ssr = seed + 0x402u;
    state.spc = seed + 0x403u;
    state.vbr = seed + 0x404u;
    state.sgr = seed + 0x405u;
    state.dbr = seed + 0x406u;
    state.macl = seed + 0x407u;
    state.mach = seed + 0x408u;
    state.pr = seed + 0x409u;
    state.fpscr = fpscr;
    state.fpul = seed + 0x40Au;
    return state;
}

void append_state(std::vector<std::uint8_t>& bytes,
                  const std::uint32_t tag,
                  const SstState& state) {
    put_u32(bytes, static_cast<std::uint32_t>(katana::testing::sh4_sst_state_chunk_size));
    put_u32(bytes, tag);
    for (const auto value : state.r)
        put_u32(bytes, value);
    for (const auto value : state.r_bank)
        put_u32(bytes, value);
    for (const auto value : state.fp0)
        put_u32(bytes, value);
    for (const auto value : state.fp1)
        put_u32(bytes, value);
    put_u32(bytes, state.pc);
    put_u32(bytes, state.gbr);
    put_u32(bytes, state.sr);
    put_u32(bytes, state.ssr);
    put_u32(bytes, state.spc);
    put_u32(bytes, state.vbr);
    put_u32(bytes, state.sgr);
    put_u32(bytes, state.dbr);
    put_u32(bytes, state.macl);
    put_u32(bytes, state.mach);
    put_u32(bytes, state.pr);
    put_u32(bytes, state.fpscr);
    put_u32(bytes, state.fpul);
}

struct SyntheticFixture {
    SstState initial;
    SstState final;
    std::vector<std::uint8_t> bytes;
};

SyntheticFixture make_fixture() {
    constexpr auto sr = katana::runtime::sr_md_mask | katana::runtime::sr_rb_mask |
                        katana::runtime::sr_t_mask | katana::runtime::sr_s_mask |
                        katana::runtime::sr_q_mask | katana::runtime::sr_m_mask;
    constexpr auto fpscr = katana::runtime::fpscr_fr_mask | katana::runtime::fpscr_dn_mask;
    SyntheticFixture fixture{
        make_state(0x10000000u, sr, fpscr), make_state(0x20000000u, sr, fpscr), {}};
    put_u32(fixture.bytes, static_cast<std::uint32_t>(katana::testing::sh4_sst_record_size));
    append_state(fixture.bytes, 1u, fixture.initial);
    append_state(fixture.bytes, 2u, fixture.final);
    put_u32(fixture.bytes, static_cast<std::uint32_t>(katana::testing::sh4_sst_cycle_chunk_size));
    put_u32(fixture.bytes, 3u);
    put_u32(fixture.bytes, 4u);

    const auto append_cycle = [&](const std::uint32_t actions,
                                  const std::uint32_t write_size,
                                  const std::uint32_t read_size) {
        put_u32(fixture.bytes, actions);
        put_u32(fixture.bytes, fixture.initial.pc);
        put_u32(fixture.bytes, 0x00000009u);
        put_u32(fixture.bytes, 0x11223344u);
        put_u64(fixture.bytes, 0x1122334455667788ull);
        put_u32(fixture.bytes, write_size);
        put_u32(fixture.bytes, 0x55667788u);
        put_u64(fixture.bytes, 0x8877665544332211ull);
        put_u32(fixture.bytes, read_size);
    };
    append_cycle(4u, 3u, 7u);
    append_cycle(4u, 3u, 7u);
    append_cycle(5u, 3u, 4u);
    append_cycle(6u, 8u, 7u);

    put_u32(fixture.bytes, static_cast<std::uint32_t>(katana::testing::sh4_sst_opcode_chunk_size));
    put_u32(fixture.bytes, 4u);
    for (const auto opcode :
         std::array<std::uint32_t, 5u>{0x0009u, 0x6123u, 0x311Cu, 0x0009u, 0x322Cu}) {
        put_u32(fixture.bytes, opcode);
    }
    require(fixture.bytes.size() == katana::testing::sh4_sst_record_size,
            "Synthetische Fixture besitzt falsche Groesse.");
    return fixture;
}

template <typename Operation>
void require_corpus_invalid(Operation&& operation, const char* const message) {
    bool rejected = false;
    try {
        operation();
    } catch (const SstCorpusInvalid& error) {
        rejected = error.classification() == ResultClassification::CorpusInvalid;
    }
    require(rejected, message);
}

void parser_tests() {
    constexpr std::string_view filename = "0110nnnnmmmm0011_sz0_pr0.json.bin";
    const auto fixture = make_fixture();
    const auto parsed =
        katana::testing::parse_sh4_sst_bytes(fixture.bytes, filename, SstParserOptions{1u, true});
    require(parsed.filename == filename && parsed.cases.size() == 1u,
            "Gueltige SST-Fixture wird nicht gelesen.");
    const auto& test = parsed.cases.front();
    require(test.initial == fixture.initial && test.final == fixture.final,
            "SST-State mit FPUL wurde nicht vollstaendig gelesen.");
    require(test.cycles[0].write_size == 3u && test.cycles[0].read_size == 7u,
            "Inaktive Garbage-Felder wurden beim Lesen veraendert.");
    require(test.cycles[2].has_read() && test.cycles[2].read_size == 4u &&
                test.cycles[3].has_write() && test.cycles[3].write_size == 8u,
            "Aktive Cycle-Zugriffe wurden falsch dekodiert.");
    require(test.opcodes[1] == 0x6123u,
            "Testopcode wurde nicht als expliziter 16-Bit-Wert gelesen.");

    auto corrupt = fixture.bytes;
    overwrite_u32(corrupt, 0u, 787u);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Falsche Recordlaenge wird akzeptiert.");

    corrupt = fixture.bytes;
    overwrite_u32(corrupt, 8u, 9u);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Falscher State-Tag wird akzeptiert.");

    corrupt = fixture.bytes;
    overwrite_u32(corrupt, 580u, 3u);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Falsche Cycle-Anzahl wird akzeptiert.");

    corrupt = fixture.bytes;
    overwrite_u32(corrupt, 584u, 12u);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Unbekannte Aktionsbits werden akzeptiert.");

    corrupt = fixture.bytes;
    overwrite_u32(corrupt, 712u, 3u);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Aktive ungueltige Lesebreite wird akzeptiert.");

    corrupt = fixture.bytes;
    overwrite_u32(corrupt, 772u, 0x7123u);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Opcode widerspricht unbemerkt dem Dateinamensmuster.");

    corrupt = fixture.bytes;
    overwrite_u32(corrupt, 280u, fixture.initial.fpscr | katana::runtime::fpscr_pr_mask);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Dateiname widerspricht unbemerkt FPSCR.PR.");

    corrupt = fixture.bytes;
    corrupt.pop_back();
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Abgeschnittene SST-Datei wird akzeptiert.");

    corrupt = fixture.bytes;
    corrupt.push_back(0u);
    require_corpus_invalid(
        [&] {
            static_cast<void>(katana::testing::parse_sh4_sst_bytes(corrupt, filename, {1u, true}));
        },
        "Nachlaufende SST-Daten werden akzeptiert.");

    require_corpus_invalid(
        [&] {
            static_cast<void>(
                katana::testing::parse_sh4_sst_bytes(fixture.bytes, "bad.json.bin", {1u, true}));
        },
        "Ungueltiger SST-Dateiname wird akzeptiert.");

    bool infrastructure = false;
    try {
        static_cast<void>(katana::testing::parse_sh4_sst_file(
            std::filesystem::path("missing-sh4-sst-fixture.json.bin"), {1u, true}));
    } catch (const katana::testing::SstInfrastructureError& error) {
        infrastructure = error.classification() == ResultClassification::InfrastructureError;
    }
    require(infrastructure, "Fehlende Corpusdatei ist kein infrastructure-error.");
}

struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write_file(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Temporaere Manifestdatei konnte nicht angelegt werden.");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output)
        throw std::runtime_error("Temporaere Manifestdatei konnte nicht geschrieben werden.");
}

void manifest_tests() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryDirectory temporary{std::filesystem::temp_directory_path() /
                                 ("katana-sh4-sst-" + unique)};
    std::filesystem::create_directories(temporary.path);
    write_file(temporary.path / "b.json.bin", "second");
    write_file(temporary.path / "a.json.bin", "first");
    write_file(temporary.path / "README.md", "ignored");

    const auto manifest = katana::testing::calculate_sh4_sst_manifest(temporary.path);
    const auto first_hash = katana::io::sha256_bytes("first");
    const auto second_hash = katana::io::sha256_bytes("second");
    const auto expected_text =
        "a.json.bin\t5\t" + first_hash + "\n" + "b.json.bin\t6\t" + second_hash + "\n";
    require(manifest.entries.size() == 2u && manifest.entries[0].filename == "a.json.bin" &&
                manifest.entries[1].filename == "b.json.bin",
            "SST-Manifest ist nicht ordinal nach Dateiname sortiert.");
    require(manifest.canonical_text == expected_text &&
                manifest.sha256 == katana::io::sha256_bytes(expected_text),
            "SST-Manifest verwendet nicht das kanonische UTF-8/LF-Schema.");
    require(katana::testing::sh4_sst_expected_manifest_sha256 ==
                "155ddb446f00e6e4985ea0bb978cef8984e7835c864134b33d99e33af47b46c7",
            "Gepinnter SST-Manifesthash ist falsch.");
}

void state_adapter_tests() {
    constexpr auto sr = katana::runtime::sr_md_mask | katana::runtime::sr_rb_mask |
                        katana::runtime::sr_t_mask | katana::runtime::sr_s_mask |
                        katana::runtime::sr_q_mask | katana::runtime::sr_m_mask;
    constexpr auto fpscr = katana::runtime::fpscr_fr_mask | katana::runtime::fpscr_pr_mask |
                           katana::runtime::fpscr_sz_mask | katana::runtime::fpscr_dn_mask | 1u;
    const auto source = make_state(0x30000000u, sr, fpscr);
    katana::runtime::CpuState cpu;
    const auto canaries = katana::testing::initialize_sst_internal_canaries(cpu);
    const auto setup = katana::testing::import_and_verify_sst_state(source, cpu);
    katana::testing::require_sst_setup_round_trip(setup);
    require(setup.matches() && katana::testing::export_sst_state(cpu) == source,
            "SST-State roundtrippt nicht exakt.");
    require(cpu.r == source.r && cpu.r_bank == source.r_bank && cpu.fr == source.fp1 &&
                cpu.xf == source.fp0,
            "RB/MD- oder physische FR-Bankabbildung ist falsch.");
    require(cpu.t && cpu.s && cpu.q && cpu.m && cpu.privileged_mode() &&
                cpu.register_bank_selected() && cpu.fpu_double_precision() &&
                cpu.fpu_transfer_pair() && cpu.fpu_register_bank_selected(),
            "SR/FPSCR-Modusbits wurden nicht vollstaendig importiert.");
    require(katana::testing::compare_sst_internal_state(canaries, cpu).matches(),
            "State-Import veraendert Katana-interne Canaries.");

    cpu.attempted_guest_instructions += katana::testing::sh4_sst_cycle_count;
    cpu.retired_guest_instructions += katana::testing::sh4_sst_cycle_count;
    cpu.pending_guest_cycles += 7u;
    cpu.active_instruction_pc = 0x8C010006u;
    cpu.active_instruction_physical_pc = 0x0C010006u;
    const auto successful_execution = katana::testing::compare_sst_internal_state_after_success(
        canaries, cpu, {7u, cpu.active_instruction_pc, cpu.active_instruction_physical_pc});
    require(successful_execution.matches(),
            "Zulaessiger SST-Ausfuehrungsfortschritt wird als Seitenwirkung gewertet.");
    --cpu.retired_guest_instructions;
    require(!katana::testing::compare_sst_internal_state_after_success(
                 canaries, cpu, {7u, cpu.active_instruction_pc, cpu.active_instruction_physical_pc})
                 .matches(),
            "SST-Erfolgsadapter akzeptiert weniger als vier retirte Instruktionen.");
    ++cpu.retired_guest_instructions;

    const auto physical_fp0 = source.fp0;
    const auto physical_fp1 = source.fp1;
    cpu.toggle_fpu_register_bank();
    const auto toggled = katana::testing::export_sst_state(cpu);
    require(toggled.fp0 == physical_fp0 && toggled.fp1 == physical_fp1 &&
                (toggled.fpscr & katana::runtime::fpscr_fr_mask) == 0u,
            "FR-Umschaltung verliert die physischen FP0/FP1-Baenke.");

    cpu.tea ^= 1u;
    const auto internal_difference = katana::testing::compare_sst_internal_state(canaries, cpu);
    require(!internal_difference.matches() &&
                internal_difference.differences.front().path == "internal.tea",
            "Unerwartete interne Seitenwirkung wird nicht erkannt.");

    auto expected = source;
    auto rounded = source;
    expected.fp0[0] = 0x3F800000u;
    rounded.fp0[0] = 0x3F800001u;
    require(!katana::testing::compare_sst_states(expected, rounded, FpuComparisonMode::Strict)
                    .matches() &&
                katana::testing::compare_sst_states(
                    expected, rounded, FpuComparisonMode::UpstreamCompatible)
                    .matches(),
            "Strict und upstream-compatible FPU-Vergleich sind nicht getrennt.");
}

void waiver_tests() {
    const std::string valid =
        "{\"schema\":\"katana-sh4-sst-waivers\",\"version\":1,"
        "\"corpus_commit\":\"48975cb1a9569abb5a0cba587013ea54edf79100\",\"waivers\":["
        "{\"corpus_commit\":\"48975cb1a9569abb5a0cba587013ea54edf79100\","
        "\"filename\":\"0011nnnnmmmm1100_sz0_pr0.json.bin\","
        "\"case_indices\":[1,4],"
        "\"classification\":\"not-applicable-reference-known-bug\","
        "\"reason\":\"Gepinnte Referenzluecke\","
        "\"evidence\":\"Unabhaengiger ISA-Vergleich\"},"
        "{\"corpus_commit\":\"48975cb1a9569abb5a0cba587013ea54edf79100\","
        "\"filename\":\"0011nnnnmmmm1100_sz0_pr0.json.bin\","
        "\"case_range\":{\"first\":10,\"last\":12},"
        "\"classification\":\"not-applicable-katana-restricted\","
        "\"reason\":\"Deklarierte Runtimegrenze\","
        "\"evidence\":\"ISA-Bericht restricted\"}]}";
    const auto parsed = katana::testing::parse_sh4_sst_waivers_json(valid, "sh4-sst-waivers.json");
    require(
        parsed.version == 1u && parsed.corpus_commit == katana::testing::sh4_sst_corpus_commit &&
            parsed.waivers.size() == 2u &&
            parsed.waivers[0].case_indices == std::vector<std::uint32_t>{1u, 4u} &&
            parsed.waivers[1].case_range.has_value() && parsed.waivers[1].case_range->last == 12u,
        "Gueltige versionierte SST-Waiver werden nicht gelesen.");

    auto stale = valid;
    stale.replace(stale.find("48975cb1"), 8u, "00000000");
    require_corpus_invalid(
        [&] {
            static_cast<void>(
                katana::testing::parse_sh4_sst_waivers_json(stale, "sh4-sst-waivers.json"));
        },
        "Waiver eines anderen Corpus-Commits bleibt gueltig.");

    auto forbidden = valid;
    forbidden.replace(forbidden.find("not-applicable-reference-known-bug"),
                      std::string("not-applicable-reference-known-bug").size(),
                      "fail-state");
    require_corpus_invalid(
        [&] {
            static_cast<void>(
                katana::testing::parse_sh4_sst_waivers_json(forbidden, "sh4-sst-waivers.json"));
        },
        "Waiver darf eine echte Fail-Klassifikation verschleiern.");

    const std::string overlap =
        "{\"schema\":\"katana-sh4-sst-waivers\",\"version\":1,"
        "\"corpus_commit\":\"48975cb1a9569abb5a0cba587013ea54edf79100\",\"waivers\":["
        "{\"corpus_commit\":\"48975cb1a9569abb5a0cba587013ea54edf79100\","
        "\"filename\":\"0011nnnnmmmm1100_sz0_pr0.json.bin\","
        "\"case_range\":{\"first\":1,\"last\":3},"
        "\"classification\":\"not-applicable-reference-known-bug\","
        "\"reason\":\"A\",\"evidence\":\"B\"},"
        "{\"corpus_commit\":\"48975cb1a9569abb5a0cba587013ea54edf79100\","
        "\"filename\":\"0011nnnnmmmm1100_sz0_pr0.json.bin\","
        "\"case_indices\":[3],"
        "\"classification\":\"not-applicable-reference-known-bug\","
        "\"reason\":\"C\",\"evidence\":\"D\"}]}";
    require_corpus_invalid(
        [&] {
            static_cast<void>(
                katana::testing::parse_sh4_sst_waivers_json(overlap, "sh4-sst-waivers.json"));
        },
        "Ueberlappende Waiver werden akzeptiert.");

    const auto checked_in =
        katana::testing::parse_sh4_sst_waivers_file("tests/sh4_sst/waivers.json");
    require(checked_in.waivers.size() == 1u,
            "Eingecheckte Waiver-Datei enthaelt nicht exakt die bekannte Referenzluecke.");
    const auto& known_div1_bug = checked_in.waivers.front();
    require(checked_in.corpus_commit == katana::testing::sh4_sst_corpus_commit &&
                known_div1_bug.filename == "0011nnnnmmmm0100_sz0_pr0.json.bin" &&
                known_div1_bug.classification ==
                    ResultClassification::NotApplicableReferenceKnownBug &&
                known_div1_bug.case_indices.size() == 43u &&
                known_div1_bug.case_indices.front() == 2u &&
                known_div1_bug.case_indices.back() == 494u && !known_div1_bug.evidence.empty(),
            "Eingecheckter DIV1-Referenzwaiver verletzt den exakten Parservertrag.");
}

void report_tests() {
    katana::testing::SstReportBasis report;
    report.katana_commit = "0123456789012345678901234567890123456789";
    report.corpus_manifest_sha256 =
        "155ddb446f00e6e4985ea0bb978cef8984e7835c864134b33d99e33af47b46c7";
    report.compiler = "MSVC test";
    report.build_type = "Debug";
    report.host_platform = "synthetic";
    report.runtime_abi = 7u;
    report.backend_abi = 8u;
    report.backend_profile_version = 1u;
    report.generated_native_code_forms = 233u;
    report.scope = "smoke";
    report.selection.expected_scope_vectors =
        katana::sh4::external_evidence_contract::smoke_vector_count;
    report.total_vectors = 2u;
    report.applicable_vectors = 1u;
    report.passed_vectors = 1u;
    report.failed_vectors = 0u;
    report.classifications = {
        {ResultClassification::Pass, 1u},
        {ResultClassification::NotApplicableKatanaRestricted, 1u},
    };
    report.used_files = {"b.json.bin", "a.json.bin"};
    report.represented_opcodes = {0x300Cu, 0x2009u};
    const auto json = katana::testing::format_sh4_sst_report_json(report);
    require(json.find("\"schema\":\"katana-sh4-sst-conformance\"") != std::string::npos &&
                json.find("\"report_version\":1") != std::string::npos &&
                json.find("\"katana_commit\":") != std::string::npos &&
                json.find("\"corpus_commit\":") != std::string::npos &&
                json.find("\"corpus_manifest_sha256\":") != std::string::npos &&
                json.find("\"runtime_abi\":7") != std::string::npos &&
                json.find("\"backend_abi\":8") != std::string::npos &&
                json.find("\"backend_profile\":\"external-conformance\"") != std::string::npos &&
                json.find("\"backend_profile_version\":1") != std::string::npos &&
                json.find("\"generated_native_code_forms\":233") != std::string::npos &&
                json.find("\"scope\":\"smoke\"") != std::string::npos &&
                json.find("\"selection\":{\"complete_scope\":false,"
                          "\"expected_scope_vectors\":65") != std::string::npos &&
                json.find("\"counts\":{\"total\":2,\"applicable\":1,\"passed\":1,"
                          "\"failed\":0}") != std::string::npos &&
                json.find("C:\\\\") == std::string::npos,
            "Maschinenlesbarer SST-Bericht verliert Evidence-Schluesselfelder.");

    report.failed_vectors = 1u;
    bool harness_invalid = false;
    try {
        static_cast<void>(katana::testing::format_sh4_sst_report_json(report));
    } catch (const katana::testing::SstHarnessInvalid& error) {
        harness_invalid = error.classification() == ResultClassification::HarnessInvalid;
    }
    require(harness_invalid, "Widerspruechliche Berichtscounts werden akzeptiert.");
}

void classification_tests() {
    require(
        std::string_view(katana::testing::result_classification_name(
            ResultClassification::FailDelaySlot)) == "fail-delay-slot" &&
            std::string_view(katana::testing::result_classification_name(
                ResultClassification::NotApplicableAccessShape)) == "not-applicable-access-shape" &&
            std::string_view(katana::testing::memory_profile_name(
                katana::testing::MemoryProfile::NativeProductMemory)) == "native-product-memory",
        "Stabile SST-Klassifikationsnamen fehlen.");
}

} // namespace

int main() {
    try {
        parser_tests();
        manifest_tests();
        state_adapter_tests();
        waiver_tests();
        report_tests();
        classification_tests();
        std::cout << "SH4-SST Parser-, Manifest- und State-Adaptertests erfolgreich.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
