#pragma once

#include "katana/runtime/runtime.hpp"
#include "katana/sh4/external_evidence_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace katana::testing {

inline constexpr std::string_view sh4_sst_corpus_commit =
    katana::sh4::external_evidence_contract::corpus_commit;
inline constexpr std::string_view sh4_sst_corpus_tree = "b5ca6e7a14976dd3eae6ebb5a06b305da221ae09";
inline constexpr std::string_view sh4_sst_expected_manifest_sha256 =
    katana::sh4::external_evidence_contract::corpus_manifest_sha256;
inline constexpr std::string_view sh4_sst_filename_regex =
    R"(^[01nmid]{16}_sz[01]_pr[01]\.json\.bin$)";

inline constexpr std::size_t sh4_sst_state_word_count = 69u;
inline constexpr std::size_t sh4_sst_cycle_count = 4u;
inline constexpr std::size_t sh4_sst_opcode_count = 5u;
inline constexpr std::size_t sh4_sst_state_chunk_size = 284u;
inline constexpr std::size_t sh4_sst_cycle_entry_size = 44u;
inline constexpr std::size_t sh4_sst_cycle_chunk_size = 188u;
inline constexpr std::size_t sh4_sst_opcode_chunk_size = 28u;
inline constexpr std::size_t sh4_sst_record_size = 788u;
inline constexpr std::size_t sh4_sst_corpus_records_per_file = 500u;
inline constexpr std::size_t sh4_sst_corpus_file_count = 233u;
inline constexpr std::size_t sh4_sst_corpus_file_size =
    sh4_sst_corpus_records_per_file * sh4_sst_record_size;

enum class ResultClassification : std::uint8_t {
    Pass,
    FailState,
    FailControlFlow,
    FailDelaySlot,
    FailMemoryAddress,
    FailMemoryWidth,
    FailMemoryValue,
    FailMemoryOrder,
    FailExtraSideEffect,
    FailUnboundTarget,
    FailUnexpectedException,
    NotApplicableReferenceAlignment,
    NotApplicableReferenceException,
    NotApplicableReferenceMmio,
    NotApplicableReferenceKnownBug,
    NotApplicableKatanaRestricted,
    NotApplicableAccessShape,
    CorpusInvalid,
    HarnessInvalid,
    InfrastructureError,
};

enum class MemoryProfile : std::uint8_t {
    NativeProductMemory,
    FlatSemanticMemory,
};

enum class FpuComparisonMode : std::uint8_t {
    Strict,
    UpstreamCompatible,
};

[[nodiscard]] const char* result_classification_name(ResultClassification value) noexcept;
[[nodiscard]] const char* memory_profile_name(MemoryProfile value) noexcept;
[[nodiscard]] const char* fpu_comparison_mode_name(FpuComparisonMode value) noexcept;

class SstError : public std::runtime_error {
  public:
    SstError(ResultClassification classification,
             std::string message,
             std::string filename = {},
             std::optional<std::size_t> offset = std::nullopt);

    [[nodiscard]] ResultClassification classification() const noexcept;
    [[nodiscard]] const std::string& filename() const noexcept;
    [[nodiscard]] const std::optional<std::size_t>& offset() const noexcept;

  private:
    ResultClassification classification_ = ResultClassification::InfrastructureError;
    std::string filename_;
    std::optional<std::size_t> offset_;
};

class SstCorpusInvalid final : public SstError {
  public:
    explicit SstCorpusInvalid(std::string message,
                              std::string filename = {},
                              std::optional<std::size_t> offset = std::nullopt);
};

class SstHarnessInvalid final : public SstError {
  public:
    explicit SstHarnessInvalid(std::string message);
};

class SstInfrastructureError final : public SstError {
  public:
    explicit SstInfrastructureError(std::string message, std::string filename = {});
};

struct SstState {
    std::array<std::uint32_t, 16u> r{};
    std::array<std::uint32_t, 8u> r_bank{};
    std::array<std::uint32_t, 16u> fp0{};
    std::array<std::uint32_t, 16u> fp1{};
    std::uint32_t pc = 0u;
    std::uint32_t gbr = 0u;
    std::uint32_t sr = 0u;
    std::uint32_t ssr = 0u;
    std::uint32_t spc = 0u;
    std::uint32_t vbr = 0u;
    std::uint32_t sgr = 0u;
    std::uint32_t dbr = 0u;
    std::uint32_t macl = 0u;
    std::uint32_t mach = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t fpul = 0u;

    bool operator==(const SstState&) const = default;
};

struct SstCycle {
    static constexpr std::uint32_t read_action = 1u;
    static constexpr std::uint32_t write_action = 2u;
    static constexpr std::uint32_t fetch_action = 4u;
    static constexpr std::uint32_t valid_actions = read_action | write_action | fetch_action;

    std::uint32_t actions = 0u;
    std::uint32_t fetch_address = 0u;
    std::uint32_t fetch_value = 0u;
    std::uint32_t write_address = 0u;
    std::uint64_t write_value = 0u;
    std::uint32_t write_size = 0u;
    std::uint32_t read_address = 0u;
    std::uint64_t read_value = 0u;
    std::uint32_t read_size = 0u;

    [[nodiscard]] bool has_read() const noexcept {
        return (actions & read_action) != 0u;
    }
    [[nodiscard]] bool has_write() const noexcept {
        return (actions & write_action) != 0u;
    }
    [[nodiscard]] bool has_fetch() const noexcept {
        return (actions & fetch_action) != 0u;
    }

    bool operator==(const SstCycle&) const = default;
};

struct SstTestCase {
    SstState initial;
    SstState final;
    std::array<SstCycle, sh4_sst_cycle_count> cycles{};
    std::array<std::uint16_t, sh4_sst_opcode_count> opcodes{};

    bool operator==(const SstTestCase&) const = default;
};

struct SstCorpusFile {
    std::string filename;
    std::vector<SstTestCase> cases;
};

struct SstParserOptions {
    std::size_t expected_record_count = sh4_sst_corpus_records_per_file;
    bool validate_filename = true;
};

[[nodiscard]] SstCorpusFile parse_sh4_sst_bytes(std::span<const std::uint8_t> bytes,
                                                std::string_view filename,
                                                SstParserOptions options = {});
[[nodiscard]] SstCorpusFile parse_sh4_sst_file(const std::filesystem::path& path,
                                               SstParserOptions options = {});

struct SstManifestEntry {
    std::string filename;
    std::uint64_t size = 0u;
    std::string sha256;

    bool operator==(const SstManifestEntry&) const = default;
};

struct SstManifest {
    std::vector<SstManifestEntry> entries;
    std::string canonical_text;
    std::string sha256;
};

[[nodiscard]] SstManifest calculate_sh4_sst_manifest(const std::filesystem::path& directory);
void require_pinned_sh4_sst_manifest(const SstManifest& manifest);

struct SstStateDifference {
    std::string path;
    std::uint64_t expected = 0u;
    std::uint64_t actual = 0u;

    bool operator==(const SstStateDifference&) const = default;
};

struct SstStateComparison {
    std::vector<SstStateDifference> differences;

    [[nodiscard]] bool matches() const noexcept {
        return differences.empty();
    }
};

void import_sst_state(const SstState& source, runtime::CpuState& destination) noexcept;
[[nodiscard]] SstState export_sst_state(const runtime::CpuState& source) noexcept;
[[nodiscard]] SstStateComparison
compare_sst_states(const SstState& expected,
                   const SstState& actual,
                   FpuComparisonMode fpu_mode = FpuComparisonMode::Strict);
[[nodiscard]] SstStateComparison import_and_verify_sst_state(const SstState& source,
                                                             runtime::CpuState& destination);
void require_sst_setup_round_trip(const SstStateComparison& comparison);

struct SstInternalStateSnapshot {
    std::uint32_t tra = 0u;
    std::uint32_t tea = 0u;
    std::uint32_t expevt = 0u;
    std::uint32_t intevt = 0u;
    std::uint32_t pteh = 0u;
    std::uint32_t ptel = 0u;
    std::uint32_t ptea = 0u;
    std::uint32_t ttb = 0u;
    std::uint32_t mmucr = 0u;
    std::array<runtime::Sh4TlbEntry, 64u> utlb{};
    std::uint64_t tlb_load_count = 0u;
    bool trap_pending = false;
    std::uint64_t exception_generation = 0u;
    runtime::ExceptionCause last_exception_cause = runtime::ExceptionCause::None;
    bool exception_in_delay_slot = false;
    std::uint32_t last_exception_instruction_pc = 0u;
    std::uint32_t last_exception_instruction_physical_pc = 0u;
    std::uint32_t last_exception_owner_pc = 0u;
    std::uint64_t last_exception_generation = 0u;
    bool sleeping = false;
    std::uint32_t last_prefetch_address = 0u;
    std::uint64_t prefetch_count = 0u;
    std::uint64_t attempted_guest_instructions = 0u;
    std::uint64_t retired_guest_instructions = 0u;
    std::uint64_t total_guest_cycles = 0u;
    std::uint64_t pending_guest_cycles = 0u;
    std::uint32_t active_instruction_pc = 0u;
    std::uint32_t active_instruction_physical_pc = 0u;
    std::uint32_t active_block_virtual_start = 0u;
    std::uint32_t active_block_physical_start = 0u;
    std::uint32_t active_block_size = 0u;
    bool last_prefetch_was_store_queue = false;
    runtime::ManualResetSink manual_reset_sink;
    const void* address_space_identity = nullptr;
    const void* gdrom_services_identity = nullptr;
    const void* g1_bus_identity = nullptr;
    std::size_t memory_size = 0u;
    std::size_t memory_region_count = 0u;
    runtime::MemoryAlignmentPolicy memory_alignment = runtime::MemoryAlignmentPolicy::Permissive;
    runtime::MemoryLookupMode memory_lookup = runtime::MemoryLookupMode::Indexed;
    std::size_t memory_watchpoint_count = 0u;
    bool memory_trace_handler = false;
    bool memory_mmio_tracking = false;
    bool memory_mmio_trace_handler = false;
    bool memory_guest_write_observer = false;
    bool memory_guest_access_sink = false;
};

[[nodiscard]] SstInternalStateSnapshot
initialize_sst_internal_canaries(runtime::CpuState& cpu) noexcept;
[[nodiscard]] SstInternalStateSnapshot
capture_sst_internal_state(const runtime::CpuState& cpu) noexcept;
[[nodiscard]] SstStateComparison
compare_sst_internal_state(const SstInternalStateSnapshot& expected,
                           const runtime::CpuState& actual);

struct SstSuccessfulExecutionExpectation {
    std::uint64_t guest_cycle_delta = 0u;
    std::uint32_t last_instruction_pc = 0u;
    std::uint32_t last_instruction_physical_pc = 0u;
};

[[nodiscard]] SstStateComparison compare_sst_internal_state_after_success(
    const SstInternalStateSnapshot& before,
    const runtime::CpuState& after,
    const SstSuccessfulExecutionExpectation& expected_progress);

enum class SstMemoryOperation : std::uint8_t {
    Read,
    Write,
};

struct SstMemoryObservation {
    std::uint32_t executing_guest_pc = 0u;
    std::size_t order = 0u;
    SstMemoryOperation operation = SstMemoryOperation::Read;
    std::uint32_t address = 0u;
    std::uint32_t width = 0u;
    std::uint64_t value = 0u;

    bool operator==(const SstMemoryObservation&) const = default;
};

struct SstCaseRange {
    std::uint32_t first = 0u;
    std::uint32_t last = 0u;
};

struct SstWaiver {
    std::string corpus_commit;
    std::string filename;
    std::vector<std::uint32_t> case_indices;
    std::optional<SstCaseRange> case_range;
    ResultClassification classification = ResultClassification::NotApplicableReferenceKnownBug;
    std::string reason;
    std::string evidence;
};

struct SstWaiverFile {
    std::uint32_t version = 1u;
    std::string corpus_commit{sh4_sst_corpus_commit};
    std::vector<SstWaiver> waivers;
};

[[nodiscard]] SstWaiverFile parse_sh4_sst_waivers_json(std::string_view json,
                                                       std::string_view filename = {});
[[nodiscard]] SstWaiverFile parse_sh4_sst_waivers_file(const std::filesystem::path& path);

struct SstClassificationCount {
    ResultClassification classification = ResultClassification::Pass;
    std::uint64_t count = 0u;
};

struct SstCounterexample {
    std::string filename;
    std::uint32_t case_index = 0u;
    std::uint16_t opcode = 0u;
    ResultClassification classification = ResultClassification::Pass;
    std::vector<SstStateDifference> state_differences;
    std::vector<SstMemoryObservation> expected_memory;
    std::vector<SstMemoryObservation> actual_memory;
    std::string detail;
};

struct SstReportSelection {
    bool complete_scope = false;
    std::uint64_t expected_scope_vectors = 0u;
    std::optional<std::string> filename;
    std::optional<std::uint32_t> case_index;
    std::optional<std::uint16_t> opcode;
    std::optional<std::string> family;
    std::optional<std::size_t> shard;
    std::optional<std::size_t> shard_count;
    bool fail_fast = false;
};

struct SstReportBasis {
    std::string katana_commit;
    std::string corpus_commit{sh4_sst_corpus_commit};
    std::string corpus_manifest_sha256;
    std::string compiler;
    std::string build_type;
    std::string host_platform;
    bool lto = false;
    std::uint32_t runtime_abi = 0u;
    std::uint32_t backend_abi = 0u;
    std::string backend_profile{"external-conformance"};
    std::uint32_t backend_profile_version = 0u;
    std::uint64_t generated_native_code_forms = 0u;
    std::string scope;
    MemoryProfile memory_profile = MemoryProfile::FlatSemanticMemory;
    FpuComparisonMode fpu_comparison = FpuComparisonMode::Strict;
    SstReportSelection selection;
    std::uint64_t total_vectors = 0u;
    std::uint64_t applicable_vectors = 0u;
    std::uint64_t passed_vectors = 0u;
    std::uint64_t failed_vectors = 0u;
    std::vector<SstClassificationCount> classifications;
    std::vector<std::string> used_files;
    std::vector<std::uint16_t> represented_opcodes;
    std::vector<std::uint16_t> katana_opcodes_without_external_evidence;
    std::vector<SstWaiver> waivers;
    std::vector<SstCounterexample> first_counterexamples;
};

[[nodiscard]] std::string format_sh4_sst_report_json(const SstReportBasis& report);

} // namespace katana::testing
