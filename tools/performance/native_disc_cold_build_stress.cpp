#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/codegen/latent_aot_registry.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/codegen/partition.hpp"
#include "katana/codegen/port_export.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/iso9660.hpp"
#include "guarded_native_entry_shape.hpp"
#include "structured_control_flow_progress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t root_schema_version = 2u;
constexpr std::size_t root_header_size = 24u;
constexpr std::size_t root_record_size = 20u;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("KR-4974-Stress: " + message);
}

void require(const bool condition, const std::string& message) {
    if (!condition) fail(message);
}

void stage(const std::string_view message) {
    std::cerr << message << '\n' << std::flush;
}

void export_progress(const std::string_view phase) {
    static std::mutex mutex;
    static std::string previous;
    const std::scoped_lock lock(mutex);
    if (phase == previous) return;
    previous = phase;
    std::cerr << "[export] " << phase << '\n' << std::flush;
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4u)
        fail("abgeschnittenes Little-Endian-Feld");
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

[[nodiscard]] std::string read_regular_text(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        fail("Eingabe ist keine regulaere Nicht-Symlink-Datei: " +
             path.filename().string());
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        fail("Eingabe kann nicht geoeffnet werden: " +
             path.filename().string());
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > 64u * 1024u * 1024u)
        fail("Eingabe ist ungueltig oder zu gross: " +
             path.filename().string());
    std::string result(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input && !result.empty())
        fail("Eingabe kann nicht vollstaendig gelesen werden: " +
             path.filename().string());
    return result;
}

[[nodiscard]] std::uint64_t parse_unsigned(
    const std::string_view value,
    const std::string_view field) {
    std::uint64_t result = 0u;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result, 10);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size())
        fail("ungueltiges Dezimalfeld " + std::string(field));
    return result;
}

[[nodiscard]] std::size_t json_unsigned(
    const std::string_view json,
    const std::string_view key) {
    const auto marker = '"' + std::string(key) + "\":";
    const auto position = json.find(marker);
    if (position == std::string_view::npos)
        fail("Portmetadaten enthalten kein Feld " + std::string(key));
    auto begin = position + marker.size();
    while (begin < json.size() && (json[begin] == ' ' || json[begin] == '\t'))
        ++begin;
    auto end = begin;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
        ++end;
    const auto value = parse_unsigned(json.substr(begin, end - begin), key);
    if (value > std::numeric_limits<std::size_t>::max())
        fail("Portmetadatenfeld ist auf diesem Host zu gross");
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::size_t count_occurrences(
    const std::string_view text,
    const std::string_view needle) {
    require(!needle.empty(), "leere Suchnadel");
    std::size_t count = 0u;
    std::size_t offset = 0u;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

struct RootRecord final {
    std::uint32_t index = 0u;
    std::uint32_t function_index = 0u;
    std::uint32_t wave = 0u;
    std::uint32_t expected_blocks = 0u;
};

struct RootTable final {
    std::vector<RootRecord> records;
    std::uint32_t wave_count = 0u;
};

[[nodiscard]] RootTable parse_roots(
    const std::span<const std::uint8_t> bytes) {
    require(bytes.size() >= root_header_size, "ROOTS.BIN ist abgeschnitten");
    require(std::string_view(
                reinterpret_cast<const char*>(bytes.data()), 8u) == "KSTRROOT",
            "ROOTS.BIN-Magic ist ungueltig");
    require(read_u32(bytes, 8u) == root_schema_version,
            "ROOTS.BIN-Version ist ungueltig");
    const auto count = read_u32(bytes, 12u);
    require(count != 0u, "ROOTS.BIN besitzt keine echten Roots");
    require(read_u32(bytes, 16u) == root_record_size,
            "ROOTS.BIN-Recordgroesse ist ungueltig");
    const auto expected_size = root_header_size +
        static_cast<std::uint64_t>(count) * root_record_size;
    require(expected_size == bytes.size(), "ROOTS.BIN-Groesse ist ungueltig");

    RootTable result;
    result.wave_count = read_u32(bytes, 20u);
    require(result.wave_count != 0u && result.wave_count <= count,
            "ROOTS.BIN-Wellenzahl ist ungueltig");
    std::vector<std::size_t> wave_sizes(result.wave_count, 0u);
    std::set<std::uint32_t> functions;
    result.records.reserve(count);
    std::uint32_t previous_wave = 0u;
    for (std::uint32_t index = 0u; index < count; ++index) {
        const auto offset = root_header_size +
            static_cast<std::size_t>(index) * root_record_size;
        RootRecord record{
            read_u32(bytes, offset),
            read_u32(bytes, offset + 4u),
            read_u32(bytes, offset + 8u),
            read_u32(bytes, offset + 12u)};
        const auto reserved = read_u32(bytes, offset + 16u);
        require(record.index == index, "ROOTS.BIN-Indexfolge driftet");
        require(record.wave < result.wave_count,
                "ROOTS.BIN verweist auf eine unbekannte Seedwelle");
        require(index == 0u || record.wave >= previous_wave,
                "ROOTS.BIN-Seedwellen sind nicht monoton");
        require(record.expected_blocks != 0u && reserved == 0u,
                "ROOTS.BIN besitzt keinen reinen Realanalysevertrag");
        require(functions.insert(record.function_index).second,
                "ROOTS.BIN dupliziert eine Funktion statt den Workset zu vergroessern");
        previous_wave = record.wave;
        ++wave_sizes[record.wave];
        result.records.push_back(record);
    }
    require(std::all_of(wave_sizes.begin(), wave_sizes.end(),
                        [](const auto count_value) { return count_value != 0u; }),
            "ROOTS.BIN enthaelt eine leere Seedwelle");
    return result;
}

[[nodiscard]] std::vector<katana::codegen::LatentAotEntryHint>
parse_latent_hints(const std::string_view text) {
    std::vector<katana::codegen::LatentAotEntryHint> result;
    std::size_t begin = 0u;
    while (begin < text.size()) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        const auto line = text.substr(begin, end - begin);
        if (!line.empty()) {
            const auto at = line.find('@');
            const auto first_colon = line.find(':', at + 1u);
            const auto second_colon = line.find(':', first_colon + 1u);
            require(line.starts_with("sha256:") && at == 71u &&
                        first_colon != std::string_view::npos &&
                        second_colon != std::string_view::npos,
                    "latent-aot-entries.txt besitzt ein ungueltiges Format");
            const auto identity = line.substr(0u, at);
            require(identity.size() == 71u &&
                        std::all_of(identity.begin() + 7, identity.end(),
                                    [](const char value) {
                                        return (value >= '0' && value <= '9') ||
                                               (value >= 'a' && value <= 'f');
                                    }),
                    "Latent-AOT-SHA-256 ist ungueltig");
            const auto disc_offset = parse_unsigned(
                line.substr(at + 1u, first_colon - at - 1u), "disc_offset");
            const auto byte_size = parse_unsigned(
                line.substr(first_colon + 1u,
                            second_colon - first_colon - 1u),
                "byte_size");
            const auto entry_offset = parse_unsigned(
                line.substr(second_colon + 1u), "entry_offset");
            require(byte_size != 0u &&
                        byte_size <= std::numeric_limits<std::uint32_t>::max() &&
                        entry_offset < byte_size &&
                        entry_offset <= std::numeric_limits<std::uint32_t>::max(),
                    "Latent-AOT-Extent ist ungueltig");
            result.push_back({std::string(identity),
                              disc_offset,
                              static_cast<std::uint32_t>(byte_size),
                              static_cast<std::uint32_t>(entry_offset)});
        }
        if (newline == std::string_view::npos) break;
        begin = newline + 1u;
    }
    require(!result.empty(), "Latent-AOT-Hintliste ist leer");
    auto canonical = result;
    std::sort(canonical.begin(), canonical.end(), [](const auto& left, const auto& right) {
        return std::tie(left.byte_identity,
                        left.disc_byte_offset,
                        left.byte_size,
                        left.module_relative_offset) <
               std::tie(right.byte_identity,
                        right.disc_byte_offset,
                        right.byte_size,
                        right.module_relative_offset);
    });
    require(canonical == result &&
                std::adjacent_find(result.begin(), result.end()) == result.end(),
            "Latent-AOT-Hints sind nicht kanonisch eindeutig sortiert");
    return result;
}

struct StressProfile final {
    std::string_view name;
    std::size_t function_count = 0u;
    std::size_t root_count = 0u;
    std::size_t wave_count = 0u;
    std::size_t partition_count = 0u;
    std::size_t replay_passes = 0u;
};

[[nodiscard]] StressProfile profile_for(const std::size_t functions) {
    if (functions == 16u) return {"smoke", 16u, 14u, 4u, 4u, 1u};
    if (functions == 1'600u)
        return {"reference", 1'600u, 1'400u, 8u, 64u, 53u};
    fail("unbekanntes Fixture-Profil");
}

[[nodiscard]] std::size_t block_count(
    const std::span<const katana::ir::Function> program) {
    std::size_t result = 0u;
    for (const auto& function : program) result += function.blocks.size();
    return result;
}

[[nodiscard]] std::string canonical_partition_plan(
    const StressProfile& profile,
    const std::span<const katana::ir::Function> program,
    const std::span<const katana::codegen::TranslationUnitPartition> partitions) {
    require(partitions.size() == profile.partition_count,
            "produktive Bootpartitionierung weicht vom Profil ab");
    require(program.size() % partitions.size() == 0u,
            "Bootpartitionen teilen die Funktionen nicht exakt");
    const auto functions_per_partition = program.size() / partitions.size();
    const auto blocks = block_count(program);
    require(blocks % program.size() == 0u,
            "Fixture-Funktionen besitzen keine einheitliche Blockzahl");
    const auto blocks_per_function = blocks / program.size();

    std::ostringstream output;
    output << "{\n"
           << "  \"block_count\": " << blocks << ",\n"
           << "  \"blocks_per_function\": " << blocks_per_function << ",\n"
           << "  \"function_count\": " << program.size() << ",\n"
           << "  \"partition_count\": " << partitions.size() << ",\n"
           << "  \"partitions\": [\n";
    for (std::size_t index = 0u; index < partitions.size(); ++index) {
        const auto& partition = partitions[index];
        const auto begin = index * functions_per_partition;
        const auto end = begin + functions_per_partition;
        require(partition.index == index &&
                    partition.function_indices.size() == functions_per_partition,
                "produktive Bootpartition besitzt eine falsche Funktionsmenge");
        std::size_t partition_blocks = 0u;
        for (std::size_t position = 0u;
             position < partition.function_indices.size(); ++position) {
            require(partition.function_indices[position] == begin + position,
                    "produktive Bootpartition besitzt keinen kanonischen Bereich");
            partition_blocks += program[begin + position].blocks.size();
        }
        output << "    {\n"
               << "      \"block_count\": " << partition_blocks << ",\n"
               << "      \"function_begin\": " << begin << ",\n"
               << "      \"function_count\": " << functions_per_partition << ",\n"
               << "      \"function_end\": " << end << ",\n"
               << "      \"index\": " << index << "\n"
               << "    }" << (index + 1u == partitions.size() ? "\n" : ",\n");
    }
    output << "  ],\n"
           << "  \"profile\": \"" << profile.name << "\",\n"
           << "  \"schema\": \"katana-stress-partition-plan-v2\",\n"
           << "  \"version\": 2\n"
           << "}\n";
    return output.str();
}

struct IncrementalEpochLedger final {
    std::size_t analysis_epochs_published = 0u;
    std::size_t analysis_epochs_discarded = 0u;
    std::size_t incremental_epochs_started = 0u;
    std::size_t resolution_root_artifacts_total = 0u;
    std::size_t resolution_root_artifacts_reused = 0u;
    std::size_t resolution_root_artifacts_recomputed = 0u;
    std::size_t resolution_root_artifacts_retained = 0u;
    std::size_t resolution_epoch_retained_bytes = 0u;
    katana::analysis::ResolutionRetentionLimitReason
        resolution_retention_limit_reason =
            katana::analysis::ResolutionRetentionLimitReason::None;
    std::size_t dirty_sccs = 0u;
    std::size_t dirty_functions = 0u;
    std::size_t dirty_inventory_sinks = 0u;
    std::size_t full_cpu_recompute_fallbacks = 0u;

    bool operator==(const IncrementalEpochLedger&) const = default;

    void add(const IncrementalEpochLedger& other) {
        analysis_epochs_published += other.analysis_epochs_published;
        analysis_epochs_discarded += other.analysis_epochs_discarded;
        incremental_epochs_started += other.incremental_epochs_started;
        resolution_root_artifacts_total +=
            other.resolution_root_artifacts_total;
        resolution_root_artifacts_reused +=
            other.resolution_root_artifacts_reused;
        resolution_root_artifacts_recomputed +=
            other.resolution_root_artifacts_recomputed;
        resolution_root_artifacts_retained +=
            other.resolution_root_artifacts_retained;
        resolution_epoch_retained_bytes +=
            other.resolution_epoch_retained_bytes;
        if (other.resolution_retention_limit_reason !=
            katana::analysis::ResolutionRetentionLimitReason::None) {
            require(
                resolution_retention_limit_reason ==
                        katana::analysis::ResolutionRetentionLimitReason::None ||
                    resolution_retention_limit_reason ==
                        other.resolution_retention_limit_reason,
                "inkrementeller Epochenledger mischte verschiedene "
                "Retention-Limitgruende");
            resolution_retention_limit_reason =
                other.resolution_retention_limit_reason;
        }
        dirty_sccs += other.dirty_sccs;
        dirty_functions += other.dirty_functions;
        dirty_inventory_sinks += other.dirty_inventory_sinks;
        full_cpu_recompute_fallbacks +=
            other.full_cpu_recompute_fallbacks;
    }

    [[nodiscard]] bool balanced() const noexcept {
        return incremental_epochs_started ==
                   analysis_epochs_published +
                       analysis_epochs_discarded &&
               resolution_root_artifacts_total ==
                   resolution_root_artifacts_reused +
                       resolution_root_artifacts_recomputed;
    }

    [[nodiscard]] bool completed_without_fallback() const noexcept {
        return balanced() && incremental_epochs_started != 0u &&
               analysis_epochs_discarded == 0u &&
               full_cpu_recompute_fallbacks == 0u &&
               resolution_retention_limit_reason ==
                   katana::analysis::ResolutionRetentionLimitReason::None &&
               resolution_root_artifacts_retained <=
                   resolution_root_artifacts_total &&
               (resolution_root_artifacts_retained == 0u ||
                resolution_epoch_retained_bytes != 0u);
    }

    [[nodiscard]] bool partially_reused() const noexcept {
        return completed_without_fallback() &&
               resolution_root_artifacts_reused != 0u &&
               resolution_root_artifacts_recomputed != 0u;
    }

    [[nodiscard]] bool exact_replay() const noexcept {
        return completed_without_fallback() &&
               resolution_root_artifacts_total != 0u &&
               resolution_root_artifacts_reused ==
                   resolution_root_artifacts_total &&
               resolution_root_artifacts_recomputed == 0u &&
               dirty_sccs == 0u && dirty_functions == 0u &&
               dirty_inventory_sinks == 0u;
    }

    [[nodiscard]] bool targeted_change_observed() const noexcept {
        return partially_reused() && dirty_sccs != 0u &&
               dirty_functions != 0u && dirty_inventory_sinks != 0u;
    }
};

[[nodiscard]] IncrementalEpochLedger incremental_epoch_ledger(
    const katana::analysis::FunctionValueAnalysisProgress& progress) {
    return {
        progress.analysis_epochs_published,
        progress.analysis_epochs_discarded,
        progress.incremental_epochs_started,
        progress.resolution_root_artifacts_total,
        progress.resolution_root_artifacts_reused,
        progress.resolution_root_artifacts_recomputed,
        progress.resolution_root_artifacts_retained,
        progress.resolution_epoch_retained_bytes,
        progress.resolution_retention_limit_reason,
        progress.dirty_sccs,
        progress.dirty_functions,
        progress.dirty_inventory_sinks,
        progress.full_cpu_recompute_fallbacks};
}

[[nodiscard]] IncrementalEpochLedger incremental_epoch_ledger(
    const katana::analysis::ControlFlowAnalysisProgress& progress) {
    return {
        progress.function_value_analysis_epochs_published,
        progress.function_value_analysis_epochs_discarded,
        progress.function_value_incremental_epochs_started,
        progress.function_value_resolution_root_artifacts_total,
        progress.function_value_resolution_root_artifacts_reused,
        progress.function_value_resolution_root_artifacts_recomputed,
        progress.function_value_resolution_root_artifacts_retained,
        progress.function_value_resolution_epoch_retained_bytes,
        progress.function_value_resolution_retention_limit_reason,
        progress.function_value_dirty_sccs,
        progress.function_value_dirty_functions,
        progress.function_value_dirty_inventory_sinks,
        progress.function_value_full_cpu_recompute_fallbacks};
}

void append_incremental_epoch_counters(
    katana::ProgressCounterSnapshot& counters,
    const IncrementalEpochLedger& ledger) {
    counters.analysis_epochs_published =
        ledger.analysis_epochs_published;
    counters.analysis_epochs_discarded =
        ledger.analysis_epochs_discarded;
    counters.incremental_epochs_started =
        ledger.incremental_epochs_started;
    counters.resolution_root_artifacts_total =
        ledger.resolution_root_artifacts_total;
    counters.resolution_root_artifacts_reused =
        ledger.resolution_root_artifacts_reused;
    counters.resolution_root_artifacts_recomputed =
        ledger.resolution_root_artifacts_recomputed;
    counters.resolution_root_artifacts_retained =
        ledger.resolution_root_artifacts_retained;
    counters.resolution_epoch_retained_bytes =
        ledger.resolution_epoch_retained_bytes;
    counters.resolution_retention_limit_reason = std::string{
        katana::analysis::resolution_retention_limit_reason_name(
            ledger.resolution_retention_limit_reason)};
    counters.dirty_sccs = ledger.dirty_sccs;
    counters.dirty_functions = ledger.dirty_functions;
    counters.dirty_inventory_sinks = ledger.dirty_inventory_sinks;
    counters.full_cpu_recompute_fallbacks =
        ledger.full_cpu_recompute_fallbacks;
}

void append_executor_counters(
    katana::ProgressCounterSnapshot& counters,
    const katana::analysis::FunctionValueAnalysisProgress& progress) {
    counters.executor_running_workers =
        progress.executor_running_workers;
    counters.executor_waiting_workers =
        progress.executor_waiting_workers;
    counters.executor_idle_workers = progress.executor_idle_workers;
    counters.executor_queued_work = progress.executor_queued_work;
    counters.executor_memory_blocked_work =
        progress.executor_memory_blocked_work;
    counters.executor_continuations = progress.executor_continuations;
    counters.analysis_memory_capacity_bytes =
        progress.analysis_memory_capacity_bytes;
    counters.analysis_memory_used_bytes =
        progress.analysis_memory_used_bytes;
    counters.analysis_memory_peak_bytes =
        progress.analysis_memory_peak_bytes;
}

void append_executor_counters(
    katana::ProgressCounterSnapshot& counters,
    const katana::analysis::ParallelWorkExecutorSnapshot& snapshot) {
    counters.executor_running_workers = snapshot.running;
    counters.executor_waiting_workers = snapshot.waiting;
    counters.executor_idle_workers = snapshot.idle;
    counters.executor_queued_work = snapshot.queued;
    counters.executor_memory_blocked_work = snapshot.memory_blocked;
    counters.executor_continuations = snapshot.continuations;
    counters.analysis_memory_capacity_bytes = snapshot.memory_capacity;
    counters.analysis_memory_used_bytes = snapshot.memory_used;
    counters.analysis_memory_peak_bytes = snapshot.memory_peak;
}

struct SeedRoundLedger final {
    std::size_t seed_facts_added = 0u;
    std::size_t seed_targets_changed = 0u;
    std::size_t decode_targets = 0u;
    std::size_t metadata_targets = 0u;
    std::size_t full_cpu_fallbacks = 0u;

    bool operator==(const SeedRoundLedger&) const = default;
};

struct PersistentWorkLedger final {
    katana::analysis::PersistentAnalysisBypassReason bypass_reason =
        katana::analysis::PersistentAnalysisBypassReason::None;
    std::size_t program_delta_entries_visited = 0u;
    std::size_t function_edge_full_scans = 0u;
    std::size_t function_edge_full_sorts = 0u;
    std::size_t candidate_call_edge_full_scans = 0u;
    std::size_t candidate_call_edge_full_sorts = 0u;
    std::size_t candidate_tail_edge_full_scans = 0u;
    std::size_t candidate_tail_edge_full_sorts = 0u;
    std::size_t program_graph_blocks_built = 0u;
    std::size_t program_graph_blocks_reused = 0u;
    std::size_t program_graph_sccs_built = 0u;
    std::size_t program_graph_sccs_reused = 0u;
    std::size_t inventory_topology_entries_visited = 0u;
    std::size_t resolution_preparation_entries_visited = 0u;
    std::size_t resolution_dependency_nodes_built = 0u;
    std::size_t resolution_dependency_nodes_reused = 0u;
    std::size_t resolution_dependency_sccs_built = 0u;
    std::size_t resolution_dependency_sccs_reused = 0u;
    std::size_t abi_contract_entries_visited = 0u;
    std::size_t abi_contract_entries_rebuilt = 0u;
    std::size_t summary_candidate_entries_visited = 0u;
    std::size_t summary_candidate_entries_rebuilt = 0u;
    std::size_t final_materialized_blocks = 0u;
    std::size_t final_materialized_functions = 0u;

    [[nodiscard]] bool avoided_full_edge_prepass() const noexcept {
        return function_edge_full_scans == 0u &&
               function_edge_full_sorts == 0u &&
               candidate_call_edge_full_scans == 0u &&
               candidate_call_edge_full_sorts == 0u &&
               candidate_tail_edge_full_scans == 0u &&
               candidate_tail_edge_full_sorts == 0u;
    }

    bool operator==(const PersistentWorkLedger&) const = default;

    void add(const PersistentWorkLedger& other) {
        if (bypass_reason ==
                katana::analysis::PersistentAnalysisBypassReason::None &&
            other.bypass_reason !=
                katana::analysis::PersistentAnalysisBypassReason::None)
            bypass_reason = other.bypass_reason;
        program_delta_entries_visited +=
            other.program_delta_entries_visited;
        function_edge_full_scans += other.function_edge_full_scans;
        function_edge_full_sorts += other.function_edge_full_sorts;
        candidate_call_edge_full_scans +=
            other.candidate_call_edge_full_scans;
        candidate_call_edge_full_sorts +=
            other.candidate_call_edge_full_sorts;
        candidate_tail_edge_full_scans +=
            other.candidate_tail_edge_full_scans;
        candidate_tail_edge_full_sorts +=
            other.candidate_tail_edge_full_sorts;
        program_graph_blocks_built += other.program_graph_blocks_built;
        program_graph_blocks_reused += other.program_graph_blocks_reused;
        program_graph_sccs_built += other.program_graph_sccs_built;
        program_graph_sccs_reused += other.program_graph_sccs_reused;
        inventory_topology_entries_visited +=
            other.inventory_topology_entries_visited;
        resolution_preparation_entries_visited +=
            other.resolution_preparation_entries_visited;
        resolution_dependency_nodes_built +=
            other.resolution_dependency_nodes_built;
        resolution_dependency_nodes_reused +=
            other.resolution_dependency_nodes_reused;
        resolution_dependency_sccs_built +=
            other.resolution_dependency_sccs_built;
        resolution_dependency_sccs_reused +=
            other.resolution_dependency_sccs_reused;
        abi_contract_entries_visited +=
            other.abi_contract_entries_visited;
        abi_contract_entries_rebuilt +=
            other.abi_contract_entries_rebuilt;
        summary_candidate_entries_visited +=
            other.summary_candidate_entries_visited;
        summary_candidate_entries_rebuilt +=
            other.summary_candidate_entries_rebuilt;
        final_materialized_blocks += other.final_materialized_blocks;
        final_materialized_functions +=
            other.final_materialized_functions;
    }
};

[[nodiscard]] PersistentWorkLedger persistent_work_ledger(
    const katana::analysis::FunctionValueAnalysisProgress& progress) {
    return {
        progress.persistent_analysis_bypass_reason,
        progress.program_delta_entries_visited,
        progress.function_edge_full_scans,
        progress.function_edge_full_sorts,
        progress.candidate_call_edge_full_scans,
        progress.candidate_call_edge_full_sorts,
        progress.candidate_tail_edge_full_scans,
        progress.candidate_tail_edge_full_sorts,
        progress.program_graph_blocks_built,
        progress.program_graph_blocks_reused,
        progress.program_graph_sccs_built,
        progress.program_graph_sccs_reused,
        progress.inventory_topology_entries_visited,
        progress.resolution_preparation_entries_visited,
        progress.resolution_dependency_nodes_built,
        progress.resolution_dependency_nodes_reused,
        progress.resolution_dependency_sccs_built,
        progress.resolution_dependency_sccs_reused,
        progress.abi_contract_entries_visited,
        progress.abi_contract_entries_rebuilt,
        progress.summary_candidate_entries_visited,
        progress.summary_candidate_entries_rebuilt,
        progress.final_materialized_blocks,
        progress.final_materialized_functions};
}

[[nodiscard]] PersistentWorkLedger persistent_work_ledger(
    const katana::analysis::ControlFlowAnalysisProgress& progress) {
    return {
        progress.function_value_persistent_analysis_bypass_reason,
        progress.function_value_program_delta_entries_visited,
        progress.function_value_function_edge_full_scans,
        progress.function_value_function_edge_full_sorts,
        progress.function_value_candidate_call_edge_full_scans,
        progress.function_value_candidate_call_edge_full_sorts,
        progress.function_value_candidate_tail_edge_full_scans,
        progress.function_value_candidate_tail_edge_full_sorts,
        progress.function_value_graph_blocks_built,
        progress.function_value_graph_blocks_reused,
        progress.function_value_graph_sccs_built,
        progress.function_value_graph_sccs_reused,
        progress.function_value_inventory_topology_entries_visited,
        progress.function_value_resolution_preparation_entries_visited,
        progress.function_value_resolution_dependency_nodes_built,
        progress.function_value_resolution_dependency_nodes_reused,
        progress.function_value_resolution_dependency_sccs_built,
        progress.function_value_resolution_dependency_sccs_reused,
        progress.function_value_abi_contract_entries_visited,
        progress.function_value_abi_contract_entries_rebuilt,
        progress.function_value_summary_candidate_entries_visited,
        progress.function_value_summary_candidate_entries_rebuilt,
        progress.function_value_final_materialized_blocks,
        progress.function_value_final_materialized_functions};
}

void append_persistent_work_json(
    std::ostream& output,
    const PersistentWorkLedger& work) {
    output << "{\"persistent_analysis_bypass_reason\":\""
           << katana::analysis::persistent_analysis_bypass_reason_name(
                  work.bypass_reason)
           << "\""
           << ",\"program_delta_entries_visited\":"
           << work.program_delta_entries_visited
           << ",\"function_edge_full_scans\":"
           << work.function_edge_full_scans
           << ",\"function_edge_full_sorts\":"
           << work.function_edge_full_sorts
           << ",\"candidate_call_edge_full_scans\":"
           << work.candidate_call_edge_full_scans
           << ",\"candidate_call_edge_full_sorts\":"
           << work.candidate_call_edge_full_sorts
           << ",\"candidate_tail_edge_full_scans\":"
           << work.candidate_tail_edge_full_scans
           << ",\"candidate_tail_edge_full_sorts\":"
           << work.candidate_tail_edge_full_sorts
           << ",\"program_graph_blocks_built\":"
           << work.program_graph_blocks_built
           << ",\"program_graph_blocks_reused\":"
           << work.program_graph_blocks_reused
           << ",\"program_graph_sccs_built\":"
           << work.program_graph_sccs_built
           << ",\"program_graph_sccs_reused\":"
           << work.program_graph_sccs_reused
           << ",\"inventory_topology_entries_visited\":"
           << work.inventory_topology_entries_visited
           << ",\"resolution_preparation_entries_visited\":"
           << work.resolution_preparation_entries_visited
           << ",\"resolution_dependency_nodes_built\":"
           << work.resolution_dependency_nodes_built
           << ",\"resolution_dependency_nodes_reused\":"
           << work.resolution_dependency_nodes_reused
           << ",\"resolution_dependency_sccs_built\":"
           << work.resolution_dependency_sccs_built
           << ",\"resolution_dependency_sccs_reused\":"
           << work.resolution_dependency_sccs_reused
           << ",\"abi_contract_entries_visited\":"
           << work.abi_contract_entries_visited
           << ",\"abi_contract_entries_rebuilt\":"
           << work.abi_contract_entries_rebuilt
           << ",\"summary_candidate_entries_visited\":"
           << work.summary_candidate_entries_visited
           << ",\"summary_candidate_entries_rebuilt\":"
           << work.summary_candidate_entries_rebuilt
           << ",\"final_materialized_blocks\":"
           << work.final_materialized_blocks
           << ",\"final_materialized_functions\":"
           << work.final_materialized_functions << '}';
}

[[nodiscard]] SeedRoundLedger seed_round_ledger(
    const katana::analysis::ControlFlowAnalysisProgress& progress) {
    return {progress.round_seed_facts_added,
            progress.round_seed_targets_changed,
            progress.round_decode_targets,
            progress.round_metadata_targets,
            progress.round_full_cpu_fallbacks};
}

struct RecursivePhysicalWorkLedger final {
    std::size_t trusted_snapshot_validations = 0u;
    std::size_t seed_arena_copy_items = 0u;
    std::size_t seed_arena_copy_bytes = 0u;
    std::size_t seed_arena_shift_items = 0u;
    std::size_t seed_arena_shift_bytes = 0u;
    std::size_t epoch_index_lookups = 0u;
    std::size_t epoch_index_updates = 0u;
    std::size_t terminal_epoch_fold_items = 0u;
    std::size_t seed_contract_items_visited = 0u;
    std::size_t decoded_work_items = 0u;
    std::size_t canonical_context_updates = 0u;
    std::size_t canonical_instruction_updates = 0u;
    std::size_t canonical_function_updates = 0u;
    std::size_t public_baseline_hash_bytes = 0u;
    std::size_t public_baseline_copy_items = 0u;
    std::size_t public_sort_items = 0u;
    std::size_t public_materialized_items = 0u;
    std::size_t public_materializations = 0u;

    [[nodiscard]] bool avoided_global_seed_and_public_work(
        const std::size_t changed_seed_items) const noexcept {
        return seed_arena_copy_items == changed_seed_items &&
               seed_arena_copy_bytes ==
                   changed_seed_items * sizeof(katana::analysis::AnalysisSeed) &&
               seed_arena_shift_items == 0u &&
               seed_arena_shift_bytes == 0u &&
               terminal_epoch_fold_items == 0u &&
               public_baseline_hash_bytes == 0u &&
               public_baseline_copy_items == 0u &&
               public_sort_items == 0u &&
               public_materialized_items == 0u &&
               public_materializations == 0u;
    }

    bool operator==(const RecursivePhysicalWorkLedger&) const = default;
};

[[nodiscard]] RecursivePhysicalWorkLedger recursive_physical_work_ledger(
    const katana::analysis::RecursiveAnalysisPhysicalWork& work) {
    return {work.trusted_snapshot_validations,
            work.seed_arena_copy_items,
            work.seed_arena_copy_bytes,
            work.seed_arena_shift_items,
            work.seed_arena_shift_bytes,
            work.epoch_index_lookups,
            work.epoch_index_updates,
            work.terminal_epoch_fold_items,
            work.seed_contract_items_visited,
            work.decoded_work_items,
            work.canonical_context_updates,
            work.canonical_instruction_updates,
            work.canonical_function_updates,
            work.public_baseline_hash_bytes,
            work.public_baseline_copy_items,
            work.public_sort_items,
            work.public_materialized_items,
            work.public_materializations};
}

struct CfaPhysicalWorkLedger final {
    std::size_t recursive_snapshot_epochs = 0u;
    std::size_t recursive_final_materializations = 0u;
    RecursivePhysicalWorkLedger recursive;
    std::size_t runtime_copy_instruction_visits = 0u;
    std::size_t runtime_copy_result_entries_visited = 0u;
    std::size_t runtime_copy_result_entries_rebuilt = 0u;
    std::size_t local_control_flow_instruction_visits = 0u;
    std::size_t local_control_flow_result_entries_visited = 0u;
    std::size_t local_control_flow_result_entries_rebuilt = 0u;
    std::size_t dispatch_index_entries_visited = 0u;
    std::size_t dispatch_index_entries_rebuilt = 0u;
    std::size_t jump_table_instruction_visits = 0u;
    std::size_t jump_table_result_entries_visited = 0u;
    std::size_t jump_table_result_entries_rebuilt = 0u;
    std::size_t function_boundary_entries_visited = 0u;
    std::size_t function_boundary_entries_rebuilt = 0u;
    std::size_t function_edge_family_entries_visited = 0u;
    std::size_t function_edge_family_entries_rebuilt = 0u;
    std::size_t function_edge_state_encode_items = 0u;
    std::size_t function_edge_state_copy_items = 0u;
    std::size_t function_edge_state_exact_compare_items = 0u;
    std::size_t result_index_copy_items = 0u;
    std::size_t result_index_sort_items = 0u;
    std::size_t result_index_materialized_items = 0u;

    [[nodiscard]] bool avoided_global_prepasses(
        const std::size_t changed_seed_items) const noexcept {
        return recursive_final_materializations == 0u &&
               recursive.avoided_global_seed_and_public_work(
                   changed_seed_items) &&
               function_edge_state_exact_compare_items == 0u &&
               result_index_copy_items == 0u &&
               result_index_sort_items == 0u &&
               result_index_materialized_items == 0u;
    }

    bool operator==(const CfaPhysicalWorkLedger&) const = default;
};

[[nodiscard]] CfaPhysicalWorkLedger cfa_physical_work_ledger(
    const katana::analysis::ControlFlowAnalysisProgress& progress) {
    return {progress.recursive_snapshot_epochs,
            progress.recursive_final_materializations,
            recursive_physical_work_ledger(
                progress.recursive_physical_work),
            progress.runtime_copy_instruction_visits,
            progress.runtime_copy_result_entries_visited,
            progress.runtime_copy_result_entries_rebuilt,
            progress.local_control_flow_instruction_visits,
            progress.local_control_flow_result_entries_visited,
            progress.local_control_flow_result_entries_rebuilt,
            progress.dispatch_index_entries_visited,
            progress.dispatch_index_entries_rebuilt,
            progress.jump_table_instruction_visits,
            progress.jump_table_result_entries_visited,
            progress.jump_table_result_entries_rebuilt,
            progress.function_boundary_entries_visited,
            progress.function_boundary_entries_rebuilt,
            progress.function_edge_family_entries_visited,
            progress.function_edge_family_entries_rebuilt,
            progress.function_edge_state_encode_items,
            progress.function_edge_state_copy_items,
            progress.function_edge_state_exact_compare_items,
            progress.result_index_copy_items,
            progress.result_index_sort_items,
            progress.result_index_materialized_items};
}

[[nodiscard]] std::size_t checked_work_delta(
    const std::size_t end,
    const std::size_t begin,
    const std::string_view field) {
    require(end >= begin,
            "nichtmonotoner CFA-PhysicalWork-Zaehler: " +
                std::string(field));
    return end - begin;
}

[[nodiscard]] RecursivePhysicalWorkLedger subtract_recursive_work(
    const RecursivePhysicalWorkLedger& end,
    const RecursivePhysicalWorkLedger& begin) {
    return {
        checked_work_delta(end.trusted_snapshot_validations,
                           begin.trusted_snapshot_validations,
                           "trusted_snapshot_validations"),
        checked_work_delta(end.seed_arena_copy_items,
                           begin.seed_arena_copy_items,
                           "seed_arena_copy_items"),
        checked_work_delta(end.seed_arena_copy_bytes,
                           begin.seed_arena_copy_bytes,
                           "seed_arena_copy_bytes"),
        checked_work_delta(end.seed_arena_shift_items,
                           begin.seed_arena_shift_items,
                           "seed_arena_shift_items"),
        checked_work_delta(end.seed_arena_shift_bytes,
                           begin.seed_arena_shift_bytes,
                           "seed_arena_shift_bytes"),
        checked_work_delta(end.epoch_index_lookups,
                           begin.epoch_index_lookups,
                           "epoch_index_lookups"),
        checked_work_delta(end.epoch_index_updates,
                           begin.epoch_index_updates,
                           "epoch_index_updates"),
        checked_work_delta(end.terminal_epoch_fold_items,
                           begin.terminal_epoch_fold_items,
                           "terminal_epoch_fold_items"),
        checked_work_delta(end.seed_contract_items_visited,
                           begin.seed_contract_items_visited,
                           "seed_contract_items_visited"),
        checked_work_delta(end.decoded_work_items,
                           begin.decoded_work_items,
                           "decoded_work_items"),
        checked_work_delta(end.canonical_context_updates,
                           begin.canonical_context_updates,
                           "canonical_context_updates"),
        checked_work_delta(end.canonical_instruction_updates,
                           begin.canonical_instruction_updates,
                           "canonical_instruction_updates"),
        checked_work_delta(end.canonical_function_updates,
                           begin.canonical_function_updates,
                           "canonical_function_updates"),
        checked_work_delta(end.public_baseline_hash_bytes,
                           begin.public_baseline_hash_bytes,
                           "public_baseline_hash_bytes"),
        checked_work_delta(end.public_baseline_copy_items,
                           begin.public_baseline_copy_items,
                           "public_baseline_copy_items"),
        checked_work_delta(end.public_sort_items,
                           begin.public_sort_items,
                           "public_sort_items"),
        checked_work_delta(end.public_materialized_items,
                           begin.public_materialized_items,
                           "public_materialized_items"),
        checked_work_delta(end.public_materializations,
                           begin.public_materializations,
                           "public_materializations")};
}

[[nodiscard]] CfaPhysicalWorkLedger subtract_cfa_work(
    const CfaPhysicalWorkLedger& end,
    const CfaPhysicalWorkLedger& begin) {
    return {
        checked_work_delta(end.recursive_snapshot_epochs,
                           begin.recursive_snapshot_epochs,
                           "recursive_snapshot_epochs"),
        checked_work_delta(end.recursive_final_materializations,
                           begin.recursive_final_materializations,
                           "recursive_final_materializations"),
        subtract_recursive_work(end.recursive, begin.recursive),
        checked_work_delta(end.runtime_copy_instruction_visits,
                           begin.runtime_copy_instruction_visits,
                           "runtime_copy_instruction_visits"),
        checked_work_delta(end.runtime_copy_result_entries_visited,
                           begin.runtime_copy_result_entries_visited,
                           "runtime_copy_result_entries_visited"),
        checked_work_delta(end.runtime_copy_result_entries_rebuilt,
                           begin.runtime_copy_result_entries_rebuilt,
                           "runtime_copy_result_entries_rebuilt"),
        checked_work_delta(end.local_control_flow_instruction_visits,
                           begin.local_control_flow_instruction_visits,
                           "local_control_flow_instruction_visits"),
        checked_work_delta(end.local_control_flow_result_entries_visited,
                           begin.local_control_flow_result_entries_visited,
                           "local_control_flow_result_entries_visited"),
        checked_work_delta(end.local_control_flow_result_entries_rebuilt,
                           begin.local_control_flow_result_entries_rebuilt,
                           "local_control_flow_result_entries_rebuilt"),
        checked_work_delta(end.dispatch_index_entries_visited,
                           begin.dispatch_index_entries_visited,
                           "dispatch_index_entries_visited"),
        checked_work_delta(end.dispatch_index_entries_rebuilt,
                           begin.dispatch_index_entries_rebuilt,
                           "dispatch_index_entries_rebuilt"),
        checked_work_delta(end.jump_table_instruction_visits,
                           begin.jump_table_instruction_visits,
                           "jump_table_instruction_visits"),
        checked_work_delta(end.jump_table_result_entries_visited,
                           begin.jump_table_result_entries_visited,
                           "jump_table_result_entries_visited"),
        checked_work_delta(end.jump_table_result_entries_rebuilt,
                           begin.jump_table_result_entries_rebuilt,
                           "jump_table_result_entries_rebuilt"),
        checked_work_delta(end.function_boundary_entries_visited,
                           begin.function_boundary_entries_visited,
                           "function_boundary_entries_visited"),
        checked_work_delta(end.function_boundary_entries_rebuilt,
                           begin.function_boundary_entries_rebuilt,
                           "function_boundary_entries_rebuilt"),
        checked_work_delta(end.function_edge_family_entries_visited,
                           begin.function_edge_family_entries_visited,
                           "function_edge_family_entries_visited"),
        checked_work_delta(end.function_edge_family_entries_rebuilt,
                           begin.function_edge_family_entries_rebuilt,
                           "function_edge_family_entries_rebuilt"),
        checked_work_delta(end.function_edge_state_encode_items,
                           begin.function_edge_state_encode_items,
                           "function_edge_state_encode_items"),
        checked_work_delta(end.function_edge_state_copy_items,
                           begin.function_edge_state_copy_items,
                           "function_edge_state_copy_items"),
        checked_work_delta(end.function_edge_state_exact_compare_items,
                           begin.function_edge_state_exact_compare_items,
                           "function_edge_state_exact_compare_items"),
        checked_work_delta(end.result_index_copy_items,
                           begin.result_index_copy_items,
                           "result_index_copy_items"),
        checked_work_delta(end.result_index_sort_items,
                           begin.result_index_sort_items,
                           "result_index_sort_items"),
        checked_work_delta(end.result_index_materialized_items,
                           begin.result_index_materialized_items,
                           "result_index_materialized_items")};
}

void append_recursive_physical_work_json(
    std::ostream& output,
    const RecursivePhysicalWorkLedger& work) {
    output << "{\"trusted_snapshot_validations\":"
           << work.trusted_snapshot_validations
           << ",\"seed_arena_copy_items\":"
           << work.seed_arena_copy_items
           << ",\"seed_arena_copy_bytes\":"
           << work.seed_arena_copy_bytes
           << ",\"seed_arena_shift_items\":"
           << work.seed_arena_shift_items
           << ",\"seed_arena_shift_bytes\":"
           << work.seed_arena_shift_bytes
           << ",\"epoch_index_lookups\":"
           << work.epoch_index_lookups
           << ",\"epoch_index_updates\":"
           << work.epoch_index_updates
           << ",\"terminal_epoch_fold_items\":"
           << work.terminal_epoch_fold_items
           << ",\"seed_contract_items_visited\":"
           << work.seed_contract_items_visited
           << ",\"decoded_work_items\":"
           << work.decoded_work_items
           << ",\"canonical_context_updates\":"
           << work.canonical_context_updates
           << ",\"canonical_instruction_updates\":"
           << work.canonical_instruction_updates
           << ",\"canonical_function_updates\":"
           << work.canonical_function_updates
           << ",\"public_baseline_hash_bytes\":"
           << work.public_baseline_hash_bytes
           << ",\"public_baseline_copy_items\":"
           << work.public_baseline_copy_items
           << ",\"public_sort_items\":"
           << work.public_sort_items
           << ",\"public_materialized_items\":"
           << work.public_materialized_items
           << ",\"public_materializations\":"
           << work.public_materializations << '}';
}

void append_cfa_physical_work_json(
    std::ostream& output,
    const CfaPhysicalWorkLedger& work) {
    output << "{\"recursive_snapshot_epochs\":"
           << work.recursive_snapshot_epochs
           << ",\"recursive_final_materializations\":"
           << work.recursive_final_materializations
           << ",\"recursive\":";
    append_recursive_physical_work_json(output, work.recursive);
    output << ",\"runtime_copy_instruction_visits\":"
           << work.runtime_copy_instruction_visits
           << ",\"runtime_copy_result_entries_visited\":"
           << work.runtime_copy_result_entries_visited
           << ",\"runtime_copy_result_entries_rebuilt\":"
           << work.runtime_copy_result_entries_rebuilt
           << ",\"local_control_flow_instruction_visits\":"
           << work.local_control_flow_instruction_visits
           << ",\"local_control_flow_result_entries_visited\":"
           << work.local_control_flow_result_entries_visited
           << ",\"local_control_flow_result_entries_rebuilt\":"
           << work.local_control_flow_result_entries_rebuilt
           << ",\"dispatch_index_entries_visited\":"
           << work.dispatch_index_entries_visited
           << ",\"dispatch_index_entries_rebuilt\":"
           << work.dispatch_index_entries_rebuilt
           << ",\"jump_table_instruction_visits\":"
           << work.jump_table_instruction_visits
           << ",\"jump_table_result_entries_visited\":"
           << work.jump_table_result_entries_visited
           << ",\"jump_table_result_entries_rebuilt\":"
           << work.jump_table_result_entries_rebuilt
           << ",\"function_boundary_entries_visited\":"
           << work.function_boundary_entries_visited
           << ",\"function_boundary_entries_rebuilt\":"
           << work.function_boundary_entries_rebuilt
           << ",\"function_edge_family_entries_visited\":"
           << work.function_edge_family_entries_visited
           << ",\"function_edge_family_entries_rebuilt\":"
           << work.function_edge_family_entries_rebuilt
           << ",\"function_edge_state_encode_items\":"
           << work.function_edge_state_encode_items
           << ",\"function_edge_state_copy_items\":"
           << work.function_edge_state_copy_items
           << ",\"function_edge_state_exact_compare_items\":"
           << work.function_edge_state_exact_compare_items
           << ",\"result_index_copy_items\":"
           << work.result_index_copy_items
           << ",\"result_index_sort_items\":"
           << work.result_index_sort_items
           << ",\"result_index_materialized_items\":"
           << work.result_index_materialized_items << '}';
}

struct RealFvaWaveLedger final {
    std::size_t index = 0u;
    std::size_t added_roots = 0u;
    std::size_t boundaries = 0u;
    std::size_t summaries = 0u;
    std::size_t logical_evaluations = 0u;
    std::size_t physical_evaluations = 0u;
    std::size_t cache_lookups = 0u;
    std::size_t cache_ready_hits = 0u;
    std::size_t cache_in_flight_coalesces = 0u;
    std::size_t cache_hits = 0u;
    std::size_t cache_misses = 0u;
    std::size_t cache_replay_fallback_recomputes = 0u;
    std::size_t cache_diagnostic_bypass_evaluations = 0u;
    std::size_t multi_root_context_requests = 0u;
    std::size_t multi_root_unique_contexts = 0u;
    std::size_t multi_root_ready_reuses = 0u;
    std::size_t multi_root_in_flight_reuses = 0u;
    std::size_t multi_root_provenance_links = 0u;
    std::size_t multi_root_retained_contexts = 0u;
    std::size_t multi_root_retained_payload_bytes = 0u;
    std::size_t multi_root_evictions = 0u;
    IncrementalEpochLedger incremental;
    PersistentWorkLedger work;
    std::array<std::size_t,
               katana::analysis::detail::
                   function_evaluation_cache_miss_reason_count>
        miss_reasons{};
    bool replay = false;
};

struct RealFvaStressResult final {
    katana::analysis::detail::FunctionValueAnalysisSessionStatistics statistics;
    std::size_t logical_evaluations = 0u;
    std::size_t physical_evaluations = 0u;
    std::size_t multi_root_context_requests = 0u;
    std::size_t multi_root_unique_contexts = 0u;
    std::size_t multi_root_ready_reuses = 0u;
    std::size_t multi_root_in_flight_reuses = 0u;
    std::size_t multi_root_provenance_links = 0u;
    std::size_t multi_root_retained_contexts = 0u;
    std::size_t multi_root_retained_payload_bytes = 0u;
    std::size_t multi_root_evictions = 0u;
    IncrementalEpochLedger incremental;
    PersistentWorkLedger work;
    std::size_t replay_hits = 0u;
    std::size_t replay_misses = 0u;
    std::size_t runs = 0u;
    std::vector<RealFvaWaveLedger> waves;
};

[[nodiscard]] RealFvaStressResult run_real_fva_waves(
    const katana::io::ExecutableImage& image,
    const katana::analysis::ControlFlowAnalysisResult& analysis,
    const katana::analysis::AnalysisOverrides& overrides,
    const RootTable& roots,
    const std::span<const katana::ir::Function> program,
    const std::size_t replay_passes,
    const katana::ProgressReporter& reporter) {
    const auto total_runs = roots.wave_count + replay_passes;
    auto structured_progress = reporter.begin(
        katana::ProgressOperation::FunctionValueAnalysis,
        katana::ProgressUnit::Steps,
        total_runs,
        "real-fva-seed-waves");
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes(image);
    katana::analysis::detail::FunctionValueAnalysisSession session(
        16'384u, 1'024u * 1024u * 1024u, true);
    std::vector<katana::analysis::FunctionBoundary> boundaries;
    boundaries.reserve(roots.records.size());
    std::map<
        katana::analysis::FunctionValueDependencyNodeId,
        katana::analysis::FunctionValueSummary> published_summaries;
    RealFvaStressResult aggregate;

    const auto run = [&] (
                         const std::size_t index,
                         const bool replay,
                         const std::size_t added_roots,
                         const std::span<const katana::analysis::FunctionBoundary>
                             changed_boundaries) {
        const auto has_published_baseline =
            session.published_epoch_version() != 0u;
        if (has_published_baseline) {
            katana::analysis::detail::FunctionProgramDelta delta;
            delta.kind = replay
                ? katana::analysis::detail::
                      FunctionProgramDeltaKind::Unchanged
                : katana::analysis::detail::
                      FunctionProgramDeltaKind::Exact;
            delta.result_materialization =
                katana::analysis::
                    FunctionValueResultMaterialization::DeltaOnly;
            delta.expected_published_epoch_version =
                session.published_epoch_version();
            delta.image_identity = image.analysis_instance_identity();
            delta.image_revision = image.analysis_revision();
            delta.changed_boundaries.reserve(changed_boundaries.size());
            for (const auto& boundary : changed_boundaries) {
                delta.changed_boundaries.push_back(
                    {boundary.entry_address, boundary});
            }
            session.stage_next_function_program_delta(std::move(delta));
        }
        struct ProgressSnapshot final {
            std::string phase;
            std::size_t functions = 0u;
            std::size_t summarized_functions = 0u;
            std::size_t logical_evaluations = 0u;
            std::size_t physical_evaluations = 0u;
            std::size_t session_cache_lookups = 0u;
            std::size_t session_cache_ready_hits = 0u;
            std::size_t session_cache_in_flight_coalesces = 0u;
            std::size_t session_cache_hits = 0u;
            std::size_t session_cache_misses = 0u;
            std::size_t cache_replay_fallback_recomputes = 0u;
            std::size_t cache_diagnostic_bypass_evaluations = 0u;
            std::size_t multi_root_context_requests = 0u;
            std::size_t multi_root_unique_contexts = 0u;
            std::size_t multi_root_ready_reuses = 0u;
            std::size_t multi_root_in_flight_reuses = 0u;
            std::size_t multi_root_provenance_links = 0u;
            std::size_t multi_root_retained_contexts = 0u;
            std::size_t multi_root_retained_payload_bytes = 0u;
            std::size_t multi_root_evictions = 0u;
            IncrementalEpochLedger incremental;
            PersistentWorkLedger work;
        } last_progress;
        bool saw_progress = false;
        std::string last_phase;
        auto last_console_update = std::chrono::steady_clock::now();
        const auto before = session.statistics();
        const auto result =
            katana::analysis::detail::
                analyze_function_values_with_guarded_entry_cache(
                    image,
                    analysis.recursive.instructions,
                    boundaries,
                     analysis.resolved_edges,
                     [&](const katana::analysis::FunctionValueAnalysisProgress& progress) {
                        katana::ProgressCounterSnapshot counters;
                        counters.iteration = index + 1u;
                        counters.pass = index + 1u;
                        counters.discovered = progress.functions;
                        counters.planned_work = progress.functions;
                        counters.started = progress.physical_evaluations;
                        counters.committed_work = progress.summarized_functions;
                        counters.active_workers = progress.active_workers;
                        counters.queued_work = progress.pending;
                        counters.configured_workers =
                            progress.configured_workers;
                        append_executor_counters(counters, progress);
                        counters.added_work = added_roots;
                        counters.growing_workset =
                            !replay && index != 0u && added_roots != 0u;
                        append_incremental_epoch_counters(
                            counters,
                            incremental_epoch_ledger(progress));
                        structured_progress.update(
                            index, std::move(counters));
                        last_progress.phase = std::string(progress.phase);
                        last_progress.functions = progress.functions;
                        last_progress.summarized_functions =
                            progress.summarized_functions;
                        last_progress.logical_evaluations =
                            progress.logical_evaluations;
                        last_progress.physical_evaluations =
                            progress.physical_evaluations;
                        last_progress.session_cache_lookups =
                            progress.session_cache_lookups;
                        last_progress.session_cache_ready_hits =
                            progress.session_cache_ready_hits;
                        last_progress.session_cache_in_flight_coalesces =
                            progress.session_cache_in_flight_coalesces;
                        last_progress.session_cache_hits =
                            progress.session_cache_hits;
                        last_progress.session_cache_misses =
                            progress.session_cache_misses;
                        last_progress.cache_replay_fallback_recomputes =
                            progress.cache_replay_fallback_recomputes;
                        last_progress.cache_diagnostic_bypass_evaluations =
                            progress.cache_diagnostic_bypass_evaluations;
                        last_progress.multi_root_context_requests =
                            progress.multi_root_context_requests;
                        last_progress.multi_root_unique_contexts =
                            progress.multi_root_unique_contexts;
                        last_progress.multi_root_ready_reuses =
                            progress.multi_root_ready_reuses;
                        last_progress.multi_root_in_flight_reuses =
                            progress.multi_root_in_flight_reuses;
                        last_progress.multi_root_provenance_links =
                            progress.multi_root_provenance_links;
                        last_progress.multi_root_retained_contexts =
                            progress.multi_root_retained_contexts;
                        last_progress.multi_root_retained_payload_bytes =
                            progress.multi_root_retained_payload_bytes;
                        last_progress.multi_root_evictions =
                            progress.multi_root_evictions;
                         last_progress.incremental =
                             incremental_epoch_ledger(progress);
                         last_progress.work =
                             persistent_work_ledger(progress);
                        saw_progress = true;
                        const std::array milestones{
                            std::string_view{"start"},
                            std::string_view{"blocks-complete"},
                            std::string_view{"functions-complete"},
                            std::string_view{"fixpoint-start"},
                            std::string_view{"fixpoint-complete"},
                            std::string_view{"resolution-start"},
                            std::string_view{"complete"}};
                        const auto now = std::chrono::steady_clock::now();
                        const auto milestone =
                            std::find(milestones.begin(), milestones.end(),
                                      progress.phase) != milestones.end();
                        const auto heartbeat =
                            progress.phase.find("heartbeat") !=
                                std::string_view::npos ||
                            now - last_console_update >=
                                std::chrono::seconds(5);
                        if ((milestone && progress.phase != last_phase) ||
                            heartbeat) {
                            last_phase = std::string(progress.phase);
                            last_console_update = now;
                            std::cerr << "[fva "
                                      << (replay ? "replay" : "wave") << ' '
                                      << index + 1u << '/'
                                      << roots.wave_count + replay_passes << "] "
                                      << progress.phase << " functions="
                                      << progress.summarized_functions << '/'
                                      << progress.functions << " pending="
                                      << progress.pending << " workers="
                                      << progress.active_workers << " logical="
                                      << progress.logical_evaluations << " physical="
                                      << progress.physical_evaluations
                                      << " resolution="
                                      << progress.resolution_functions_committed
                                      << '/'
                                      << progress.resolution_functions_total
                                      << " cache="
                                      << progress.session_cache_hits << '/'
                                      << progress.session_cache_lookups << '\n'
                                      << std::flush;
                        }
                    },
                    shapes,
                    session);
        require(!result.budget_exhausted,
                "reale FunctionValue-Analyse erschoepfte ihr Budget");
        require(saw_progress, "reale FunctionValue-Analyse meldete keinen Fortschritt");
        if (result.result_materialization ==
            katana::analysis::FunctionValueResultMaterialization::TerminalFull) {
            require(result.summary_replacements.empty() &&
                        result.removed_summary_shards.empty(),
                    "terminale FVA lieferte zusaetzlich Delta-Summary-Shards");
            published_summaries.clear();
            for (const auto& summary : result.summaries) {
                const katana::analysis::FunctionValueDependencyNodeId owner{
                    summary.function_address,
                    katana::analysis::FunctionValueDependencyNodeKind::Function};
                require(published_summaries.emplace(owner, summary).second,
                        "terminale FVA lieferte doppelte Summary-Owner");
            }
        } else {
            require(result.summaries.empty(),
                    "DeltaOnly-FVA materialisierte unerwartet Voll-Summaries");
            for (const auto owner : result.removed_summary_shards)
                published_summaries.erase(owner);
            for (const auto& replacement : result.summary_replacements) {
                require(replacement.owner.kind ==
                            katana::analysis::
                                FunctionValueDependencyNodeKind::Function &&
                            replacement.owner.address ==
                                replacement.summary.function_address,
                        "DeltaOnly-FVA lieferte einen falsch gebundenen "
                        "Summary-Shard");
                published_summaries.insert_or_assign(
                    replacement.owner, replacement.summary);
            }
        }
        std::set<std::uint32_t> boundary_addresses;
        std::set<std::uint32_t> summary_addresses;
        for (const auto& boundary : boundaries)
            boundary_addresses.insert(boundary.entry_address);
        for (const auto& [owner, summary] : published_summaries) {
            require(owner.kind ==
                        katana::analysis::
                            FunctionValueDependencyNodeKind::Function &&
                        owner.address == summary.function_address,
                    "publizierter FVA-Summary-Shard verlor seine Ownerbindung");
            summary_addresses.insert(summary.function_address);
        }
        require(boundary_addresses.size() == boundaries.size() &&
                    summary_addresses.size() == published_summaries.size() &&
                    boundary_addresses == summary_addresses,
                "reale FVA-Summary-Adressen entsprechen nicht exakt den Boundaries");
        const auto after = session.statistics();
        require(after.lookups >= before.lookups &&
                    after.hits >= before.hits && after.misses >= before.misses,
                "FunctionValue-Sessionzaehler liefen rueckwaerts");
        const auto lookup_delta = after.lookups - before.lookups;
        const auto hit_delta = after.hits - before.hits;
        const auto miss_delta = after.misses - before.misses;
        const auto multi_root_reuses =
            last_progress.multi_root_ready_reuses +
            last_progress.multi_root_in_flight_reuses;
        require(last_progress.phase == "complete" &&
                    last_progress.functions == boundaries.size() &&
                    last_progress.summarized_functions == boundaries.size() &&
                    last_progress.logical_evaluations ==
                        lookup_delta + multi_root_reuses +
                            last_progress.cache_diagnostic_bypass_evaluations &&
                    last_progress.session_cache_lookups == lookup_delta &&
                    last_progress.session_cache_hits == hit_delta &&
                    last_progress.session_cache_misses == miss_delta &&
                    last_progress.session_cache_ready_hits +
                            last_progress.session_cache_in_flight_coalesces ==
                        hit_delta &&
                    last_progress.physical_evaluations ==
                        miss_delta +
                            last_progress.cache_replay_fallback_recomputes +
                            last_progress.cache_diagnostic_bypass_evaluations &&
                    last_progress.multi_root_context_requests ==
                        last_progress.multi_root_unique_contexts +
                            multi_root_reuses &&
                    last_progress.multi_root_retained_contexts <=
                        last_progress.multi_root_unique_contexts &&
                    last_progress.multi_root_evictions ==
                        last_progress.multi_root_unique_contexts -
                            last_progress.multi_root_retained_contexts &&
                    ((last_progress.multi_root_retained_contexts == 0u) ==
                     (last_progress.multi_root_retained_payload_bytes == 0u)) &&
                     last_progress.incremental.completed_without_fallback() &&
                     last_progress.work.bypass_reason ==
                         katana::analysis::
                             PersistentAnalysisBypassReason::None &&
                     last_progress.cache_diagnostic_bypass_evaluations == 0u,
                 "reale FVA-Progress-/Session-/Physical-Bilanz driftet");
        if (has_published_baseline) {
            require(
                last_progress.work.avoided_full_edge_prepass(),
                "Exact-/Unchanged-Rootwelle wiederholte einen "
                "vollstaendigen Resolved-Edge-Scan oder -Sort.");
            require(
                last_progress.work.program_delta_entries_visited ==
                    changed_boundaries.size(),
                "Rootwelle besuchte nicht exakt ihr deklariertes "
                "Boundary-Delta.");
        }
        RealFvaWaveLedger ledger;
        ledger.index = index;
        ledger.added_roots = added_roots;
        ledger.boundaries = boundaries.size();
        ledger.summaries = published_summaries.size();
        ledger.logical_evaluations = last_progress.logical_evaluations;
        ledger.physical_evaluations = last_progress.physical_evaluations;
        ledger.cache_lookups = lookup_delta;
        ledger.cache_ready_hits =
            last_progress.session_cache_ready_hits;
        ledger.cache_in_flight_coalesces =
            last_progress.session_cache_in_flight_coalesces;
        ledger.cache_hits = hit_delta;
        ledger.cache_misses = miss_delta;
        ledger.cache_replay_fallback_recomputes =
            last_progress.cache_replay_fallback_recomputes;
        ledger.cache_diagnostic_bypass_evaluations =
            last_progress.cache_diagnostic_bypass_evaluations;
        ledger.multi_root_context_requests =
            last_progress.multi_root_context_requests;
        ledger.multi_root_unique_contexts =
            last_progress.multi_root_unique_contexts;
        ledger.multi_root_ready_reuses =
            last_progress.multi_root_ready_reuses;
        ledger.multi_root_in_flight_reuses =
            last_progress.multi_root_in_flight_reuses;
        ledger.multi_root_provenance_links =
            last_progress.multi_root_provenance_links;
        ledger.multi_root_retained_contexts =
            last_progress.multi_root_retained_contexts;
        ledger.multi_root_retained_payload_bytes =
            last_progress.multi_root_retained_payload_bytes;
        ledger.multi_root_evictions =
            last_progress.multi_root_evictions;
        ledger.incremental = last_progress.incremental;
        ledger.work = last_progress.work;
        for (std::size_t reason = 0u; reason < ledger.miss_reasons.size(); ++reason)
            ledger.miss_reasons[reason] =
                after.miss_reasons[reason] - before.miss_reasons[reason];
        require(std::accumulate(ledger.miss_reasons.begin(),
                                ledger.miss_reasons.end(), std::size_t{0u}) ==
                    miss_delta,
                "FVA-Welle verlor ihre primaere Missgrundbilanz");
        ledger.replay = replay;
        std::cerr << "[fva incremental " << index + 1u << '/' << total_runs
                  << "] roots="
                  << ledger.incremental.resolution_root_artifacts_reused
                  << "/"
                  << ledger.incremental.resolution_root_artifacts_total
                  << " reused, recomputed="
                  << ledger.incremental.resolution_root_artifacts_recomputed
                  << " retained="
                  << ledger.incremental.resolution_root_artifacts_retained
                  << "/"
                  << ledger.incremental.resolution_epoch_retained_bytes
                  << "B retention="
                  << katana::analysis::resolution_retention_limit_reason_name(
                         ledger.incremental
                             .resolution_retention_limit_reason)
                  << " dirty=" << ledger.incremental.dirty_sccs << '/'
                  << ledger.incremental.dirty_functions << '/'
                  << ledger.incremental.dirty_inventory_sinks
                  << " fallbacks="
                  << ledger.incremental.full_cpu_recompute_fallbacks << '\n'
                  << std::flush;
        if (!replay) {
            require(added_roots <= boundaries.size(),
                    "wachsende FVA-Welle meldete mehr neue als totale Roots");
            const auto previous_roots = boundaries.size() - added_roots;
            require(
                added_roots != 0u && miss_delta != 0u &&
                    ledger.incremental.resolution_root_artifacts_total ==
                        boundaries.size() &&
                    ledger.incremental.resolution_root_artifacts_reused ==
                        previous_roots &&
                    ledger.incremental.resolution_root_artifacts_recomputed ==
                        added_roots &&
                    ledger.incremental.dirty_sccs == added_roots &&
                    ledger.incremental.dirty_functions == added_roots &&
                    ledger.incremental.dirty_inventory_sinks == 0u,
                "wachsende unabhaengige FVA-Welle invalidierte alte Roots "
                "oder markierte mehr als die neuen Roots dirty");
        } else {
            require(added_roots == 0u && lookup_delta == hit_delta &&
                        miss_delta == 0u &&
                        last_progress.physical_evaluations == 0u &&
                        last_progress.cache_replay_fallback_recomputes == 0u &&
                        last_progress.cache_diagnostic_bypass_evaluations == 0u &&
                        ledger.incremental.exact_replay(),
                    "identischer Replay war weder physisch noch im "
                    "Root-Artefaktpfad exakt arbeitsfrei");
        }
        aggregate.waves.push_back(ledger);
        aggregate.logical_evaluations += last_progress.logical_evaluations;
        aggregate.physical_evaluations += last_progress.physical_evaluations;
        aggregate.multi_root_context_requests +=
            last_progress.multi_root_context_requests;
        aggregate.multi_root_unique_contexts +=
            last_progress.multi_root_unique_contexts;
        aggregate.multi_root_ready_reuses +=
            last_progress.multi_root_ready_reuses;
        aggregate.multi_root_in_flight_reuses +=
            last_progress.multi_root_in_flight_reuses;
        aggregate.multi_root_provenance_links +=
            last_progress.multi_root_provenance_links;
        aggregate.multi_root_retained_contexts +=
            last_progress.multi_root_retained_contexts;
        aggregate.multi_root_retained_payload_bytes +=
            last_progress.multi_root_retained_payload_bytes;
        aggregate.multi_root_evictions +=
            last_progress.multi_root_evictions;
        aggregate.incremental.add(last_progress.incremental);
        aggregate.work.add(last_progress.work);
        ++aggregate.runs;
        if (replay) {
            aggregate.replay_hits = hit_delta;
            aggregate.replay_misses = miss_delta;
        }
        structured_progress.update(index + 1u);
    };

    for (std::size_t wave = 0u; wave < roots.wave_count; ++wave) {
        std::vector<katana::analysis::FunctionBoundary>
            changed_boundaries;
        for (const auto& root : roots.records) {
            if (root.wave != wave) continue;
            require(root.function_index < overrides.functions.size(),
                    "ROOTS.BIN verweist ausserhalb der Analyse-Overrides");
            const auto& declaration = overrides.functions[root.function_index];
            const auto found = std::find_if(
                program.begin(), program.end(), [&](const auto& function) {
                    return function.entry_address == declaration.address;
                });
            require(found != program.end() &&
                        found->blocks.size() == root.expected_blocks,
                    "ROOTS.BIN passt nicht zur realen IR-Blockmenge");
            const katana::analysis::FunctionBoundary boundary{
                declaration.address, declaration.size};
            boundaries.push_back(boundary);
            changed_boundaries.push_back(boundary);
        }
        std::sort(boundaries.begin(), boundaries.end(), [](const auto& left,
                                                           const auto& right) {
            return left.entry_address < right.entry_address;
        });
        std::sort(changed_boundaries.begin(), changed_boundaries.end(),
                  [](const auto& left, const auto& right) {
                      return left.entry_address < right.entry_address;
                  });
        run(
            wave,
            false,
            std::count_if(
                roots.records.begin(),
                roots.records.end(),
                [&](const auto& root) { return root.wave == wave; }),
            changed_boundaries);
    }
    require(boundaries.size() == roots.records.size(),
            "Seedwellen materialisierten nicht alle Roots");
    require(replay_passes != 0u, "FVA-Stress besitzt keinen exakten Replay");
    for (std::size_t replay = 0u; replay < replay_passes; ++replay)
        run(
            roots.wave_count + replay,
            true,
            0u,
            std::span<const katana::analysis::FunctionBoundary>{});

    structured_progress.complete(total_runs);
    aggregate.statistics = session.statistics();
    require(aggregate.statistics.balanced() &&
                aggregate.statistics.misses == aggregate.physical_evaluations &&
                aggregate.statistics.misses != 0u &&
                aggregate.replay_misses == 0u,
            "persistente reale FVA-Session erfuellt den Replayvertrag nicht");
    return aggregate;
}

constexpr std::uint32_t semantic_base = 0x8D00'0000u;
constexpr std::uint32_t semantic_heavy_blocks = 4'096u;
constexpr std::uint32_t semantic_root_tail = semantic_heavy_blocks * 4u;

struct SemanticFvaStressResult final {
    katana::analysis::detail::FunctionValueAnalysisSessionStatistics statistics;
    std::array<std::size_t,
               katana::analysis::detail::
                   function_evaluation_cache_miss_reason_count>
        targeted_miss_reasons{};
    std::size_t logical_evaluations = 0u;
    std::size_t physical_evaluations = 0u;
    std::size_t multi_root_context_requests = 0u;
    std::size_t multi_root_unique_contexts = 0u;
    std::size_t multi_root_ready_reuses = 0u;
    std::size_t multi_root_in_flight_reuses = 0u;
    std::size_t multi_root_provenance_links = 0u;
    std::size_t multi_root_retained_contexts = 0u;
    std::size_t multi_root_retained_payload_bytes = 0u;
    std::size_t multi_root_evictions = 0u;
    IncrementalEpochLedger incremental;
    IncrementalEpochLedger targeted_incremental;
    PersistentWorkLedger work;
    PersistentWorkLedger targeted_work;
    std::size_t targeted_misses = 0u;
    std::size_t targeted_hits = 0u;
    std::size_t replay_hits = 0u;
    std::size_t cache_replay_fallback_recomputes = 0u;
    std::size_t cache_diagnostic_bypass_evaluations = 0u;
    std::size_t maximum_head_of_line_milliseconds = 0u;
    std::size_t maximum_ready_ahead = 0u;
    std::size_t strongly_connected_components = 0u;
    std::vector<std::uint32_t> targeted_miss_functions;
    std::vector<std::uint32_t> targeted_hit_only_functions;
    bool abi_stack_argument_observed = false;
    bool persistent_store_observed = false;
    bool indirect_candidate_observed = false;
};

[[nodiscard]] std::array<
    katana::analysis::ResolvedControlFlowEdge,
    5u>
semantic_baseline_edges() {
    return {{
        {semantic_base + semantic_root_tail + 0x06u,
         semantic_base + 0x5000u,
         katana::analysis::ResolvedControlFlowKind::Call,
         false,
         katana::analysis::ControlFlowEvidence::ProvenComplete,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         false},
        {semantic_base + 0x5008u,
         semantic_base + 0x5040u,
         katana::analysis::ResolvedControlFlowKind::Call,
         false,
         katana::analysis::ControlFlowEvidence::ProvenComplete,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         false},
        {semantic_base + 0x5042u,
         semantic_base + 0x5080u,
         katana::analysis::ResolvedControlFlowKind::Call,
         false,
         katana::analysis::ControlFlowEvidence::ProvenComplete,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         false},
        {semantic_base + 0x5048u,
         semantic_base + 0x5000u,
         katana::analysis::ResolvedControlFlowKind::Call,
         false,
         katana::analysis::ControlFlowEvidence::ProvenComplete,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         false},
        {semantic_base + 0x5086u,
         semantic_base + 0x50C0u,
         katana::analysis::ResolvedControlFlowKind::Call,
         false,
         katana::analysis::ControlFlowEvidence::ProvenComplete,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         false},
    }};
}

[[nodiscard]] SemanticFvaStressResult run_semantic_fva_stress(
    const std::span<const std::uint8_t> bytes,
    const katana::ProgressReporter& reporter) {
    require(bytes.size() == 0x5148u,
            "semantische Teilfixture besitzt eine falsche Groesse");
    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".kr4974-semantic",
                       semantic_base,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
    image.add_entry_point(semantic_base);
    const auto lines = katana::sh4::disassemble(bytes, semantic_base);
    const std::array<katana::analysis::FunctionBoundary, 6u> boundaries{{
        {semantic_base, semantic_root_tail + 0x0Eu},
        {semantic_base + 0x5000u, 0x10u},
        {semantic_base + 0x5040u, 0x10u},
        {semantic_base + 0x5080u, 0x0Eu},
        {semantic_base + 0x50C0u, 0x04u},
        {semantic_base + 0x5100u, 0x08u},
    }};
    const auto baseline_edges = semantic_baseline_edges();
    auto changed_edges = baseline_edges;
    changed_edges.front().target_address = semantic_base + 0x5080u;

    struct ObservedCacheDecision final {
        std::size_t run = 0u;
        katana::analysis::detail::FunctionEvaluationCacheDecision decision;
    };
    std::atomic<std::size_t> active_run{0u};
    std::mutex decisions_mutex;
    std::vector<ObservedCacheDecision> decisions;
    auto semantic_progress = reporter.begin(
        katana::ProgressOperation::FunctionValueAnalysis,
        katana::ProgressUnit::Steps,
        4u,
        "semantic-fva-invalidation");
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes(image);
    katana::analysis::detail::FunctionValueAnalysisSession session(
        16'384u,
        1'024u * 1024u * 1024u,
        true,
        [&](const katana::analysis::detail::
                FunctionEvaluationCacheDecision& decision) {
            const std::scoped_lock lock(decisions_mutex);
            decisions.push_back({
                active_run.load(std::memory_order_acquire), decision});
        });
    SemanticFvaStressResult aggregate;

    const auto run = [&](const std::span<const katana::analysis::ResolvedControlFlowEdge> edges,
                         const std::size_t run_index,
                         const bool exact_replay,
                         const bool targeted_change) {
        active_run.store(run_index, std::memory_order_release);
        if (session.published_epoch_version() != 0u) {
            katana::analysis::detail::FunctionProgramDelta delta;
            delta.kind = exact_replay
                ? katana::analysis::detail::
                      FunctionProgramDeltaKind::Unchanged
                : katana::analysis::detail::
                      FunctionProgramDeltaKind::Exact;
            delta.result_materialization = exact_replay
                ? katana::analysis::
                      FunctionValueResultMaterialization::DeltaOnly
                : katana::analysis::
                      FunctionValueResultMaterialization::TerminalFull;
            delta.expected_published_epoch_version =
                session.published_epoch_version();
            delta.image_identity = image.analysis_instance_identity();
            delta.image_revision = image.analysis_revision();
            if (!exact_replay) {
                const auto changed_site =
                    baseline_edges.front().instruction_address;
                katana::analysis::detail::FunctionProgramEdgeSiteDelta
                    changed;
                changed.instruction_address = changed_site;
                for (const auto& edge : edges) {
                    if (edge.instruction_address == changed_site &&
                        !edge.analysis_candidate_carrier)
                        changed.values.push_back(edge);
                }
                delta.changed_semantic_edge_sites.push_back(
                    std::move(changed));
            }
            session.stage_next_function_program_delta(std::move(delta));
        }
        katana::ProgressCounterSnapshot initial_counters;
        initial_counters.pass = run_index + 1u;
        initial_counters.planned_work = boundaries.size();
        initial_counters.configured_workers =
            katana::analysis::global_analysis_executor().maximum_jobs();
        const auto initial_executor =
            katana::analysis::global_analysis_executor().snapshot();
        initial_counters.active_workers = initial_executor.running;
        append_executor_counters(initial_counters, initial_executor);
        semantic_progress.update(run_index, std::move(initial_counters));
        const auto before = session.statistics();
        std::mutex run_progress_mutex;
        std::mutex observation_mutex;
        std::string terminal_phase;
        std::size_t terminal_functions = 0u;
        std::size_t terminal_summaries = 0u;
        std::size_t terminal_logical = 0u;
        std::size_t terminal_physical = 0u;
        std::size_t terminal_fallbacks = 0u;
        std::size_t terminal_diagnostic_bypasses = 0u;
        std::size_t terminal_multi_root_context_requests = 0u;
        std::size_t terminal_multi_root_unique_contexts = 0u;
        std::size_t terminal_multi_root_ready_reuses = 0u;
        std::size_t terminal_multi_root_in_flight_reuses = 0u;
        std::size_t terminal_multi_root_provenance_links = 0u;
        std::size_t terminal_multi_root_retained_contexts = 0u;
        std::size_t terminal_multi_root_retained_payload_bytes = 0u;
        std::size_t terminal_multi_root_evictions = 0u;
        IncrementalEpochLedger terminal_incremental;
        PersistentWorkLedger terminal_work;
        bool stack_observed = false;
        bool persistent_observed = false;
        const auto result =
            katana::analysis::detail::
                analyze_function_values_with_guarded_entry_cache(
                    image,
                    lines,
                    boundaries,
                    edges,
                    [&](const katana::analysis::FunctionValueAnalysisProgress& progress) {
                        const std::scoped_lock lock(run_progress_mutex);
                        terminal_phase = std::string(progress.phase);
                        terminal_functions = progress.functions;
                        terminal_summaries = progress.summarized_functions;
                        terminal_logical = progress.logical_evaluations;
                        terminal_physical = progress.physical_evaluations;
                        terminal_fallbacks =
                            progress.cache_replay_fallback_recomputes;
                        terminal_diagnostic_bypasses =
                            progress.cache_diagnostic_bypass_evaluations;
                        terminal_multi_root_context_requests =
                            progress.multi_root_context_requests;
                        terminal_multi_root_unique_contexts =
                            progress.multi_root_unique_contexts;
                        terminal_multi_root_ready_reuses =
                            progress.multi_root_ready_reuses;
                        terminal_multi_root_in_flight_reuses =
                            progress.multi_root_in_flight_reuses;
                        terminal_multi_root_provenance_links =
                            progress.multi_root_provenance_links;
                        terminal_multi_root_retained_contexts =
                            progress.multi_root_retained_contexts;
                        terminal_multi_root_retained_payload_bytes =
                            progress.multi_root_retained_payload_bytes;
                        terminal_multi_root_evictions =
                            progress.multi_root_evictions;
                         terminal_incremental =
                             incremental_epoch_ledger(progress);
                         terminal_work = persistent_work_ledger(progress);
                        aggregate.maximum_head_of_line_milliseconds = std::max(
                            aggregate.maximum_head_of_line_milliseconds,
                            progress.resolution_head_of_line_elapsed_milliseconds);
                        aggregate.maximum_ready_ahead = std::max(
                            aggregate.maximum_ready_ahead,
                            progress.resolution_functions_ready);
                        katana::ProgressCounterSnapshot counters;
                        counters.pass = run_index + 1u;
                        counters.active_workers = progress.active_workers;
                        counters.planned_work =
                            progress.resolution_functions_total;
                        counters.queued_work =
                            progress.resolution_functions_total -
                            std::min(
                                progress.resolution_functions_total,
                                progress.resolution_functions_started);
                        counters.started =
                            progress.resolution_functions_started;
                        counters.ready_work =
                            progress.resolution_functions_ready;
                        counters.committed_work =
                            progress.resolution_functions_committed;
                        counters.configured_workers =
                            progress.configured_workers;
                        append_executor_counters(counters, progress);
                        counters.head_of_line_index =
                            progress.resolution_head_of_line_index;
                        counters.head_of_line_elapsed_milliseconds =
                            progress.resolution_head_of_line_elapsed_milliseconds;
                        counters.ready_ahead =
                            progress.resolution_functions_ready;
                        counters.evaluation_requests =
                            progress.logical_evaluations;
                        counters.active_evaluation_requests =
                            progress.active_evaluation_requests;
                        counters.evaluation_request_nanoseconds =
                            progress.evaluation_request_nanoseconds;
                        counters.maximum_evaluation_request_nanoseconds =
                            progress.maximum_evaluation_request_nanoseconds;
                        counters.cache_key_builds =
                            progress.cache_key_builds;
                        counters.active_cache_key_builds =
                            progress.active_cache_key_builds;
                        counters.cache_key_build_nanoseconds =
                            progress.cache_key_build_nanoseconds;
                        counters.maximum_cache_key_build_nanoseconds =
                            progress.maximum_cache_key_build_nanoseconds;
                        counters.cache_waits = progress.cache_waits;
                        counters.active_cache_waits =
                            progress.active_cache_waits;
                        counters.cache_wait_nanoseconds =
                            progress.cache_wait_nanoseconds;
                        counters.maximum_cache_wait_nanoseconds =
                            progress.maximum_cache_wait_nanoseconds;
                        counters.cache_replays = progress.cache_replays;
                        counters.active_cache_replays =
                            progress.active_cache_replays;
                        counters.cache_replay_nanoseconds =
                            progress.cache_replay_nanoseconds;
                        counters.maximum_cache_replay_nanoseconds =
                            progress.maximum_cache_replay_nanoseconds;
                        counters.physical_evaluations =
                            progress.physical_evaluations;
                        counters.active_physical_evaluations =
                            progress.active_physical_evaluations;
                        counters.physical_evaluation_nanoseconds =
                            progress.physical_evaluation_nanoseconds;
                        counters.maximum_physical_evaluation_nanoseconds =
                            progress.maximum_physical_evaluation_nanoseconds;
                        counters.cache_commits = progress.cache_commits;
                        counters.active_cache_commits =
                            progress.active_cache_commits;
                        counters.cache_commit_nanoseconds =
                            progress.cache_commit_nanoseconds;
                        counters.maximum_cache_commit_nanoseconds =
                            progress.maximum_cache_commit_nanoseconds;
                        counters.cache_lookups =
                            progress.session_cache_lookups;
                        counters.cache_ready_hits =
                            progress.session_cache_ready_hits;
                        counters.cache_in_flight_coalesces =
                            progress.session_cache_in_flight_coalesces;
                        counters.cache_hits =
                            progress.session_cache_hits;
                        counters.cache_misses =
                            progress.session_cache_misses;
                        counters.cache_replay_fallback_recomputes =
                            progress.cache_replay_fallback_recomputes;
                        counters.cache_diagnostic_bypass_evaluations =
                            progress.cache_diagnostic_bypass_evaluations;
                        counters.multi_root_context_requests =
                            progress.multi_root_context_requests;
                        counters.multi_root_unique_contexts =
                            progress.multi_root_unique_contexts;
                        counters.multi_root_ready_reuses =
                            progress.multi_root_ready_reuses;
                        counters.multi_root_in_flight_reuses =
                            progress.multi_root_in_flight_reuses;
                        counters.multi_root_provenance_links =
                            progress.multi_root_provenance_links;
                        counters.multi_root_retained_contexts =
                            progress.multi_root_retained_contexts;
                        counters.multi_root_retained_payload_bytes =
                            progress.multi_root_retained_payload_bytes;
                        counters.multi_root_evictions =
                            progress.multi_root_evictions;
                        counters.cache_evictions =
                            progress.session_cache_evictions;
                        counters.cache_entries =
                            progress.session_cache_entries;
                        counters.cache_retained_payload_bytes =
                            progress.session_cache_retained_payload_bytes;
                        counters.cache_miss_cold =
                            progress.session_cache_miss_cold;
                        counters.cache_miss_evicted =
                            progress.session_cache_miss_evicted;
                        counters.cache_miss_oversize_or_no_exact_replay =
                            progress
                                .session_cache_miss_oversize_or_no_exact_replay;
                        counters.cache_miss_function_shape_changed =
                            progress
                                .session_cache_miss_function_shape_changed;
                        counters.cache_miss_projected_ingress_changed =
                            progress
                                .session_cache_miss_projected_ingress_changed;
                        counters.cache_miss_summary_dependency_changed =
                            progress
                                .session_cache_miss_summary_dependency_changed;
                        counters.cache_miss_abi_contract_changed =
                            progress.session_cache_miss_abi_contract_changed;
                        counters.cache_miss_resolution_lens_changed =
                            progress
                                .session_cache_miss_resolution_lens_changed;
                        counters.cache_miss_inventory_sink_changed =
                            progress
                                .session_cache_miss_inventory_sink_changed;
                        counters.cache_miss_isolation_partition_changed =
                            progress
                                .session_cache_miss_isolation_partition_changed;
                        counters.cache_miss_contextual_summary_changed =
                            progress
                                .session_cache_miss_contextual_summary_changed;
                        counters.cache_miss_tail_ingress_changed =
                            progress.session_cache_miss_tail_ingress_changed;
                        append_incremental_epoch_counters(
                            counters, terminal_incremental);
                        semantic_progress.update(
                            run_index, std::move(counters));
                    },
                    shapes,
                    session,
                    [&](const katana::analysis::detail::AbiContractObservation& observation) {
                        if (observation.function_address !=
                            semantic_base + 0x5000u)
                            return;
                        const std::scoped_lock lock(observation_mutex);
                        stack_observed = observation.stack_reads_complete &&
                            std::find(observation.stack_read_slots.begin(),
                                      observation.stack_read_slots.end(), 0) !=
                                observation.stack_read_slots.end();
                        persistent_observed =
                            observation.persistent_store_sources != 0u;
                    });
        const bool semantic_progress_complete =
            (terminal_phase == "complete" &&
             terminal_summaries == boundaries.size()) ||
            terminal_phase == "terminal-materialized";
        const auto expected_materialization = exact_replay
            ? katana::analysis::
                  FunctionValueResultMaterialization::DeltaOnly
            : katana::analysis::
                  FunctionValueResultMaterialization::TerminalFull;
        require(!result.budget_exhausted && semantic_progress_complete &&
                    result.result_materialization == expected_materialization &&
                    terminal_functions == boundaries.size(),
                "semantische FVA erreichte keinen exakten Terminalzustand");
        std::set<std::uint32_t> summaries;
        for (const auto& summary : result.summaries)
            summaries.insert(summary.function_address);
        std::set<std::uint32_t> expected;
        for (const auto& boundary : boundaries)
            expected.insert(boundary.entry_address);
        if (exact_replay) {
            const bool exact_baseline_inventory_delta =
                result.guarded_code_inventory_replacements.size() == 1u &&
                result.guarded_code_inventory_replacements.front()
                        .owner.address == 0u &&
                result.guarded_code_inventory_replacements.front()
                        .owner.kind ==
                    katana::analysis::FunctionValueDependencyNodeKind::
                        AnalysisBaseline;
            require(result.summaries.empty() && result.resolutions.empty() &&
                        result.summary_replacements.empty() &&
                        result.removed_summary_shards.empty() &&
                        result.resolution_replacements.empty() &&
                        result.removed_resolution_shards.empty() &&
                        exact_baseline_inventory_delta &&
                        result.removed_guarded_code_inventory_shards.empty(),
                    "semantischer Unchanged-Replay emittierte mehr als den "
                    "typisierten AnalysisBaseline-Shard");
        } else {
            require(summaries == expected &&
                        result.summaries.size() == boundaries.size(),
                    "semantische FVA verlor oder erfand eine Summary");
            aggregate.strongly_connected_components =
                result.strongly_connected_components;
        }
        aggregate.abi_stack_argument_observed |= stack_observed;
        aggregate.persistent_store_observed |= persistent_observed;
        aggregate.indirect_candidate_observed |= std::any_of(
            result.resolutions.begin(), result.resolutions.end(),
            [&](const auto& resolution) {
                return resolution.instruction_address ==
                           semantic_base + semantic_root_tail + 0x06u &&
                       std::find(resolution.targets.begin(),
                                 resolution.targets.end(),
                                 semantic_base + 0x5000u) !=
                           resolution.targets.end();
            });
        const auto after = session.statistics();
        const auto lookups = after.lookups - before.lookups;
        const auto hits = after.hits - before.hits;
        const auto misses = after.misses - before.misses;
        const auto terminal_multi_root_reuses =
            terminal_multi_root_ready_reuses +
            terminal_multi_root_in_flight_reuses;
        std::cerr << "[semantic balance " << run_index + 1u
                  << "] logical=" << terminal_logical
                  << " lookups=" << lookups
                  << " multi_root="
                  << terminal_multi_root_context_requests << '/'
                  << terminal_multi_root_unique_contexts << '/'
                  << terminal_multi_root_reuses
                  << " retained=" << terminal_multi_root_retained_contexts
                  << '/' << terminal_multi_root_retained_payload_bytes
                  << " diagnostic=" << terminal_diagnostic_bypasses
                  << " physical=" << terminal_physical
                  << " misses=" << misses
                  << " fallbacks=" << terminal_fallbacks
                  << " epochs="
                  << terminal_incremental.incremental_epochs_started << '/'
                  << terminal_incremental.analysis_epochs_published << '/'
                  << terminal_incremental.analysis_epochs_discarded
                  << " bypass="
                  << katana::analysis::persistent_analysis_bypass_reason_name(
                         terminal_work.bypass_reason)
                  << '\n' << std::flush;
        require(terminal_logical == lookups + terminal_multi_root_reuses +
                         terminal_diagnostic_bypasses &&
                     terminal_physical == misses + terminal_fallbacks +
                         terminal_diagnostic_bypasses &&
                    terminal_multi_root_context_requests ==
                        terminal_multi_root_unique_contexts +
                            terminal_multi_root_reuses &&
                    terminal_multi_root_retained_contexts <=
                        terminal_multi_root_unique_contexts &&
                    terminal_multi_root_evictions ==
                        terminal_multi_root_unique_contexts -
                            terminal_multi_root_retained_contexts &&
                    ((terminal_multi_root_retained_contexts == 0u) ==
                     (terminal_multi_root_retained_payload_bytes == 0u)) &&
                    terminal_incremental.completed_without_fallback() &&
                    terminal_work.bypass_reason ==
                        katana::analysis::
                            PersistentAnalysisBypassReason::None &&
                    terminal_diagnostic_bypasses == 0u,
                "semantische FVA-Cache-/Physical-Bilanz driftet");
        if (run_index != 0u) {
            require(
                terminal_work.avoided_full_edge_prepass(),
                "Exact-/Unchanged-FVA wiederholte einen vollstaendigen "
                "Resolved-Edge-Scan oder -Sort.");
        }
        if (exact_replay) {
            require(
                terminal_work.program_delta_entries_visited == 0u,
                "Unchanged-FVA besuchte trotz leerem Delta "
                "Programmjournal-Eintraege.");
        }
        if (targeted_change) {
            require(
                terminal_work.program_delta_entries_visited == 1u,
                "Gezielte FVA-Aenderung besuchte nicht exakt ihren einen "
                "Semantic-Edge-Site-Deltaeintrag.");
        }
        if (exact_replay) {
            std::cerr << "[semantic replay " << run_index + 1u
                      << "] cache=" << hits << '/' << lookups
                      << " misses=" << misses
                      << " physical=" << terminal_physical << " roots="
                      << terminal_incremental
                             .resolution_root_artifacts_reused
                      << '/'
                      << terminal_incremental
                             .resolution_root_artifacts_total
                      << " reused, recomputed="
                      << terminal_incremental
                             .resolution_root_artifacts_recomputed
                      << " dirty=" << terminal_incremental.dirty_sccs << '/'
                      << terminal_incremental.dirty_functions << '/'
                      << terminal_incremental.dirty_inventory_sinks << '\n'
                      << std::flush;
            require(lookups == hits && misses == 0u &&
                        terminal_physical == 0u && terminal_fallbacks == 0u &&
                        terminal_incremental.exact_replay(),
                    "semantischer exakter Replay war nicht physisch arbeitsfrei");
        }
        if (targeted_change) {
            std::cerr << "[semantic incremental] roots="
                      << terminal_incremental
                             .resolution_root_artifacts_reused
                      << '/'
                      << terminal_incremental
                             .resolution_root_artifacts_total
                      << " reused, recomputed="
                      << terminal_incremental
                             .resolution_root_artifacts_recomputed
                      << " retained="
                      << terminal_incremental
                             .resolution_root_artifacts_retained
                      << '/'
                      << terminal_incremental.resolution_epoch_retained_bytes
                      << "B retention="
                      << katana::analysis::
                             resolution_retention_limit_reason_name(
                                 terminal_incremental
                                     .resolution_retention_limit_reason)
                      << " dirty=" << terminal_incremental.dirty_sccs << '/'
                      << terminal_incremental.dirty_functions << '/'
                      << terminal_incremental.dirty_inventory_sinks
                      << " fallbacks="
                      << terminal_incremental.full_cpu_recompute_fallbacks
                      << '\n' << std::flush;
            require(
                terminal_incremental.targeted_change_observed(),
                "gezielte Semantikaenderung vermied keine Vollneuberechnung "
                "oder meldete ihre betroffenen SCC-/Funktions-/Inventory-"
                "Pfade nicht");
            aggregate.targeted_incremental = terminal_incremental;
            aggregate.targeted_work = terminal_work;
            aggregate.targeted_misses = misses;
            aggregate.targeted_hits = hits;
            for (std::size_t reason = 0u;
                 reason < aggregate.targeted_miss_reasons.size(); ++reason)
                aggregate.targeted_miss_reasons[reason] =
                    after.miss_reasons[reason] - before.miss_reasons[reason];
            std::set<std::uint32_t> miss_functions;
            std::set<std::uint32_t> hit_functions;
            {
                const std::scoped_lock lock(decisions_mutex);
                for (const auto& observed : decisions) {
                    if (observed.run != run_index) continue;
                    if (observed.decision.outcome ==
                        katana::analysis::detail::
                            FunctionEvaluationCacheLookupOutcome::Miss)
                        miss_functions.insert(
                            observed.decision.function_entry);
                    else
                        hit_functions.insert(
                            observed.decision.function_entry);
                }
            }
            aggregate.targeted_miss_functions.assign(
                miss_functions.begin(), miss_functions.end());
            for (const auto entry : hit_functions) {
                if (!miss_functions.contains(entry))
                    aggregate.targeted_hit_only_functions.push_back(entry);
            }
        }
        aggregate.logical_evaluations += terminal_logical;
        aggregate.physical_evaluations += terminal_physical;
        aggregate.multi_root_context_requests +=
            terminal_multi_root_context_requests;
        aggregate.multi_root_unique_contexts +=
            terminal_multi_root_unique_contexts;
        aggregate.multi_root_ready_reuses +=
            terminal_multi_root_ready_reuses;
        aggregate.multi_root_in_flight_reuses +=
            terminal_multi_root_in_flight_reuses;
        aggregate.multi_root_provenance_links +=
            terminal_multi_root_provenance_links;
        aggregate.multi_root_retained_contexts +=
            terminal_multi_root_retained_contexts;
        aggregate.multi_root_retained_payload_bytes +=
            terminal_multi_root_retained_payload_bytes;
        aggregate.multi_root_evictions +=
            terminal_multi_root_evictions;
        aggregate.incremental.add(terminal_incremental);
        aggregate.work.add(terminal_work);
        aggregate.cache_replay_fallback_recomputes += terminal_fallbacks;
        aggregate.cache_diagnostic_bypass_evaluations +=
            terminal_diagnostic_bypasses;
        if (exact_replay) aggregate.replay_hits += hits;
        semantic_progress.update(run_index + 1u);
    };

    run(baseline_edges, 0u, false, false);
    run(baseline_edges, 1u, true, false);
    run(changed_edges, 2u, false, true);
    // Restoring the baseline after a different immediately preceding epoch is
    // a semantic reversion, not an exact replay. It may reuse older cache
    // entries, but the persistent root epoch must conservatively compare
    // against and invalidate from the changed predecessor.
    run(baseline_edges, 3u, false, false);
    semantic_progress.complete(4u);
    aggregate.statistics = session.statistics();
    const auto noncold_targeted = std::accumulate(
        aggregate.targeted_miss_reasons.begin() + 1,
        aggregate.targeted_miss_reasons.end(), std::size_t{0u});
    constexpr std::array<std::size_t,
                         katana::analysis::detail::
                             function_evaluation_cache_miss_reason_count>
        expected_targeted_miss_closure{
            0u, 0u, 0u, 1u, 13u, 1u, 0u, 1u, 0u, 0u, 0u, 0u};
    const std::vector<std::uint32_t> expected_targeted_miss_functions{
        semantic_base,
        semantic_base + 0x5000u,
        semantic_base + 0x5040u,
        semantic_base + 0x5080u};
    const std::vector<std::uint32_t> expected_targeted_hit_only_functions{
        semantic_base + 0x50C0u};
    std::cerr << "[semantic] sccs="
              << aggregate.strongly_connected_components
              << " targeted_hits=" << aggregate.targeted_hits
              << " targeted_misses=" << aggregate.targeted_misses
              << " noncold=" << noncold_targeted
              << " ready_ahead=" << aggregate.maximum_ready_ahead
              << " hol_ms="
              << aggregate.maximum_head_of_line_milliseconds
              << " abi_stack=" << aggregate.abi_stack_argument_observed
              << " persistent=" << aggregate.persistent_store_observed
              << " indirect=" << aggregate.indirect_candidate_observed
              << " miss_functions=";
    for (const auto entry : aggregate.targeted_miss_functions)
        std::cerr << "0x" << std::hex << entry << std::dec << ',';
    std::cerr << " hit_only_functions=";
    for (const auto entry : aggregate.targeted_hit_only_functions)
        std::cerr << "0x" << std::hex << entry << std::dec << ',';
    std::cerr
              << " miss_reasons=";
    for (const auto count : aggregate.targeted_miss_reasons)
        std::cerr << count << ',';
    std::cerr << '\n' << std::flush;
    require(aggregate.statistics.balanced() &&
                aggregate.statistics.miss_reasons[0] != 0u &&
                aggregate.targeted_misses == 16u &&
                aggregate.targeted_hits == 6u && noncold_targeted != 0u &&
                aggregate.maximum_ready_ahead >= 2u &&
                aggregate.targeted_miss_reasons ==
                    expected_targeted_miss_closure &&
                aggregate.targeted_miss_functions ==
                    expected_targeted_miss_functions &&
                aggregate.targeted_hit_only_functions ==
                    expected_targeted_hit_only_functions &&
                aggregate.strongly_connected_components != 0u &&
                aggregate.strongly_connected_components < boundaries.size() &&
                aggregate.abi_stack_argument_observed &&
                aggregate.persistent_store_observed &&
                aggregate.indirect_candidate_observed,
            "semantischer SCC-/ABI-/Ingress-Folgeinvalidierungsvertrag fehlt");
    return aggregate;
}

[[nodiscard]] bool same_guarded_inventory_semantics(
    const katana::analysis::GuardedCodeInventory& left,
    const katana::analysis::GuardedCodeInventory& right) {
    auto left_walk = left.walk_diagnostics;
    auto right_walk = right.walk_diagnostics;
    left_walk.forwarded_store_evaluation_cache_hits = 0u;
    left_walk.forwarded_store_evaluation_cache_misses = 0u;
    right_walk.forwarded_store_evaluation_cache_hits = 0u;
    right_walk.forwarded_store_evaluation_cache_misses = 0u;
    return left.stored_code_addresses == right.stored_code_addresses &&
           left.returned_code_address_tables ==
               right.returned_code_address_tables &&
           left.raw_stored_candidate_budget ==
               right.raw_stored_candidate_budget &&
           left.raw_stored_candidate_count ==
               right.raw_stored_candidate_count &&
           left.candidate_budget == right.candidate_budget &&
           left.candidate_count == right.candidate_count &&
           left.shape_validation_work == right.shape_validation_work &&
           left.shape_validation_work_budget ==
               right.shape_validation_work_budget &&
           left.shape_budget_exceeded_candidates ==
               right.shape_budget_exceeded_candidates &&
           left.raw_stored_candidates_truncated ==
               right.raw_stored_candidates_truncated &&
           left.candidate_budget_exhausted ==
               right.candidate_budget_exhausted &&
           left.candidate_inventory_truncated ==
               right.candidate_inventory_truncated &&
           left.table_scan_truncated == right.table_scan_truncated &&
           left_walk == right_walk;
}

[[nodiscard]] bool same_fva_semantics(
    const katana::analysis::FunctionValueAnalysisResult& left,
    const katana::analysis::FunctionValueAnalysisResult& right) {
    return left.result_materialization ==
               katana::analysis::FunctionValueResultMaterialization::
                   TerminalFull &&
           right.result_materialization ==
               katana::analysis::FunctionValueResultMaterialization::
                   TerminalFull &&
           left.summary_replacements.empty() &&
           right.summary_replacements.empty() &&
           left.removed_summary_shards.empty() &&
           right.removed_summary_shards.empty() &&
           left.resolution_replacements.empty() &&
           right.resolution_replacements.empty() &&
           left.removed_resolution_shards.empty() &&
           right.removed_resolution_shards.empty() &&
           left.guarded_code_inventory_replacements.empty() &&
           right.guarded_code_inventory_replacements.empty() &&
           left.removed_guarded_code_inventory_shards.empty() &&
           right.removed_guarded_code_inventory_shards.empty() &&
           left.summaries == right.summaries &&
           left.resolutions == right.resolutions &&
           same_guarded_inventory_semantics(
               left.guarded_code_inventory,
               right.guarded_code_inventory) &&
           left.strongly_connected_components ==
               right.strongly_connected_components &&
           left.iteration_budget == right.iteration_budget &&
           left.budget_exhausted == right.budget_exhausted;
}

struct FvaDeltaScaleSample final {
    std::size_t function_count = 0u;
    std::size_t block_count = 0u;
    PersistentWorkLedger warm_work;
    PersistentWorkLedger terminal_presentation_work;
    PersistentWorkLedger fresh_work;
    IncrementalEpochLedger warm_incremental;
    bool warm_equals_fresh = false;
    bool late_candidate_observed = false;
};

struct FvaDeltaScaleGate final {
    FvaDeltaScaleSample n;
    FvaDeltaScaleSample eight_n;
};

[[nodiscard]] FvaDeltaScaleSample run_fva_delta_scale_sample(
    const std::span<const std::uint8_t> semantic_bytes,
    const std::size_t function_count) {
    constexpr std::size_t base_function_count = 6u;
    constexpr std::size_t candidate_helper_bytes = 16u;
    constexpr std::size_t filler_function_bytes = 8u;
    require(
        function_count > base_function_count,
        "O(delta)-Fixture besitzt zu wenige Funktionen");
    std::vector<std::uint8_t> bytes(
        semantic_bytes.begin(), semantic_bytes.end());
    const auto ordinary_filler_count =
        function_count - base_function_count - 1u;
    const auto candidate_helper_offset = bytes.size();
    const auto filler_offset =
        candidate_helper_offset + candidate_helper_bytes;
    bytes.resize(
        filler_offset + ordinary_filler_count * filler_function_bytes,
        0u);
    const auto put_u16 = [&](const std::size_t offset,
                             const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&](const std::size_t offset,
                             const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    // Candidate-only helper: consume the fifth ABI argument and persist it.
    // Its literal remains outside the exact function boundary, like normal
    // compiler-emitted PC-relative data.
    put_u16(candidate_helper_offset + 0u, 0x64F2u);
    put_u16(candidate_helper_offset + 2u, 0xD502u);
    put_u16(candidate_helper_offset + 4u, 0x2542u);
    put_u16(candidate_helper_offset + 6u, 0x000Bu);
    put_u16(candidate_helper_offset + 8u, 0x0009u);
    put_u16(candidate_helper_offset + 10u, 0x0009u);
    put_u32(
        candidate_helper_offset + 12u,
        semantic_base + 0x5140u);
    for (std::size_t index = 0u;
         index < ordinary_filler_count;
         ++index) {
        const auto offset = filler_offset + index * filler_function_bytes;
        bytes[offset] = 0x0Bu;
        bytes[offset + 1u] = 0x00u;
        bytes[offset + 2u] = 0x09u;
        bytes[offset + 3u] = 0x00u;
        bytes[offset + 4u] = 0x0Bu;
        bytes[offset + 5u] = 0x00u;
        bytes[offset + 6u] = 0x09u;
        bytes[offset + 7u] = 0x00u;
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".kr4978-fva-delta-scale",
                       semantic_base,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(semantic_base);
    const auto lines = katana::sh4::disassemble(
        std::span<const std::uint8_t>{bytes}, semantic_base);
    std::vector<katana::analysis::FunctionBoundary> boundaries{
        {semantic_base, semantic_root_tail + 0x0Eu},
        {semantic_base + 0x5000u, 0x10u},
        {semantic_base + 0x5040u, 0x10u},
        {semantic_base + 0x5080u, 0x0Eu},
        {semantic_base + 0x50C0u, 0x04u},
        {semantic_base + 0x5100u, 0x08u}};
    boundaries.reserve(function_count);
    const auto candidate_helper_address =
        semantic_base + static_cast<std::uint32_t>(
            candidate_helper_offset);
    boundaries.push_back({candidate_helper_address, 0x0Au});
    for (std::size_t index = 0u;
         index < ordinary_filler_count;
         ++index) {
        boundaries.push_back({
            semantic_base + static_cast<std::uint32_t>(
                filler_offset + index * filler_function_bytes),
            static_cast<std::uint32_t>(filler_function_bytes)});
    }
    require(
        boundaries.size() == function_count,
        "O(delta)-Fixture verlor deklarierte Funktionen");

    const auto baseline_array = semantic_baseline_edges();
    std::vector<katana::analysis::ResolvedControlFlowEdge> baseline_edges(
        baseline_array.begin(), baseline_array.end());
    const katana::analysis::ResolvedControlFlowEdge late_candidate{
        baseline_edges.front().instruction_address,
        candidate_helper_address,
        katana::analysis::ResolvedControlFlowKind::Call,
        true,
        katana::analysis::ControlFlowEvidence::GuardedPartial,
        {katana::analysis::AnalysisEvidenceOrigin::FunctionSummary},
        true};
    auto candidate_edges = baseline_edges;
    candidate_edges.push_back(late_candidate);

    struct Invocation final {
        katana::analysis::FunctionValueAnalysisResult result;
        PersistentWorkLedger work;
        IncrementalEpochLedger incremental;
        std::size_t blocks = 0u;
    };
    std::size_t invocation_sequence = 0u;
    const auto invoke = [&] (
                            katana::analysis::detail::
                                FunctionValueAnalysisSession& session,
                            katana::analysis::detail::
                                GuardedNativeEntryShapeCache& shapes,
                            const std::span<const katana::analysis::
                                ResolvedControlFlowEdge> edges) {
        const auto invocation_index = invocation_sequence++;
        Invocation invocation;
        std::string terminal_phase;
        invocation.result = katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                edges,
                [&](const katana::analysis::
                        FunctionValueAnalysisProgress& progress) {
                    terminal_phase = progress.phase;
                    invocation.work = persistent_work_ledger(progress);
                    invocation.incremental =
                        incremental_epoch_ledger(progress);
                    invocation.blocks = progress.blocks;
                },
                shapes,
                session);
        const bool reached_terminal =
            terminal_phase == "complete" ||
            terminal_phase == "terminal-materialized";
        if (!reached_terminal || invocation.result.budget_exhausted) {
            std::cerr << "[fva delta-scale " << function_count
                      << " invocation " << invocation_index
                      << "] terminal_phase=" << terminal_phase
                      << " budget_exhausted="
                      << invocation.result.budget_exhausted << '\n'
                      << std::flush;
        }
        require(
            reached_terminal && !invocation.result.budget_exhausted,
            "O(delta)-FVA erreichte keinen vollstaendigen Terminalzustand");
        return invocation;
    };

    katana::analysis::detail::GuardedNativeEntryShapeCache warm_shapes(
        image);
    katana::analysis::detail::FunctionValueAnalysisSession warm_session(
        16'384u, 1'024u * 1024u * 1024u, true);
    const auto baseline = invoke(
        warm_session, warm_shapes, baseline_edges);
    require(
        warm_session.published_epoch_version() != 0u,
        "O(delta)-Baseline publizierte keine persistente Epoche");
    katana::analysis::detail::FunctionProgramDelta delta;
    delta.kind =
        katana::analysis::detail::FunctionProgramDeltaKind::Exact;
    delta.result_materialization =
        katana::analysis::FunctionValueResultMaterialization::DeltaOnly;
    delta.expected_published_epoch_version =
        warm_session.published_epoch_version();
    delta.image_identity = image.analysis_instance_identity();
    delta.image_revision = image.analysis_revision();
    delta.changed_candidate_call_sites.push_back({
        late_candidate.instruction_address, {late_candidate}});
    warm_session.stage_next_function_program_delta(std::move(delta));
    const auto warm_delta = invoke(
        warm_session, warm_shapes, candidate_edges);
    katana::analysis::detail::FunctionProgramDelta terminal;
    terminal.kind =
        katana::analysis::detail::FunctionProgramDeltaKind::Unchanged;
    terminal.result_materialization =
        katana::analysis::FunctionValueResultMaterialization::TerminalFull;
    terminal.expected_published_epoch_version =
        warm_session.published_epoch_version();
    terminal.image_identity = image.analysis_instance_identity();
    terminal.image_revision = image.analysis_revision();
    warm_session.stage_next_function_program_delta(std::move(terminal));
    const auto warm_terminal = invoke(
        warm_session, warm_shapes, candidate_edges);

    katana::analysis::detail::GuardedNativeEntryShapeCache fresh_shapes(
        image);
    katana::analysis::detail::FunctionValueAnalysisSession fresh_session(
        16'384u, 1'024u * 1024u * 1024u, true);
    const auto fresh = invoke(
        fresh_session, fresh_shapes, candidate_edges);

    const auto warm_equals_fresh =
        same_fva_semantics(warm_terminal.result, fresh.result);
    const auto has_candidate_store = [&](const auto& result) {
        return std::any_of(
            result.guarded_code_inventory.stored_code_addresses.begin(),
            result.guarded_code_inventory.stored_code_addresses.end(),
            [&](const auto& candidate) {
                return candidate.target_address ==
                           semantic_base + 0x50C0u &&
                       std::find(
                           candidate.store_instruction_addresses.begin(),
                           candidate.store_instruction_addresses.end(),
                           candidate_helper_address + 4u) !=
                           candidate.store_instruction_addresses.end();
            });
    };
    const auto late_candidate_observed =
        !has_candidate_store(baseline.result) &&
        has_candidate_store(warm_terminal.result) &&
        has_candidate_store(fresh.result) &&
        warm_delta.work.summary_candidate_entries_visited != 0u &&
        warm_delta.work.summary_candidate_entries_rebuilt != 0u;
    const bool fresh_scan_contract =
        baseline.work.function_edge_full_scans == 1u &&
            baseline.work.function_edge_full_sorts == 1u &&
            baseline.work.candidate_call_edge_full_scans == 1u &&
            baseline.work.candidate_call_edge_full_sorts == 1u &&
            baseline.work.candidate_tail_edge_full_scans == 1u &&
            baseline.work.candidate_tail_edge_full_sorts == 1u &&
            fresh.work.function_edge_full_scans == 1u &&
            fresh.work.function_edge_full_sorts == 1u &&
            fresh.work.candidate_call_edge_full_scans == 1u &&
            fresh.work.candidate_call_edge_full_sorts == 1u &&
            fresh.work.candidate_tail_edge_full_scans == 1u &&
            fresh.work.candidate_tail_edge_full_sorts == 1u;
    require(
        fresh_scan_contract,
        "Frische O(delta)-Referenz belegte nicht jeden vollen "
        "Resolved-Edge-Indexvorlauf");
    const bool delta_contract =
        warm_delta.result.result_materialization ==
                katana::analysis::FunctionValueResultMaterialization::
                    DeltaOnly &&
            warm_terminal.result.result_materialization ==
                katana::analysis::FunctionValueResultMaterialization::
                    TerminalFull &&
            warm_delta.work.bypass_reason ==
                katana::analysis::PersistentAnalysisBypassReason::None &&
            warm_delta.work.avoided_full_edge_prepass() &&
            warm_delta.work.program_delta_entries_visited == 1u &&
            warm_delta.work.final_materialized_blocks == 0u &&
            warm_delta.work.final_materialized_functions == 0u &&
            warm_delta.incremental.completed_without_fallback() &&
            warm_terminal.work.bypass_reason ==
                katana::analysis::PersistentAnalysisBypassReason::None &&
            warm_terminal.work.program_delta_entries_visited == 0u &&
            warm_terminal.work.avoided_full_edge_prepass() &&
            warm_terminal.work.final_materialized_functions != 0u &&
            warm_equals_fresh && late_candidate_observed;
    if (!delta_contract) {
        std::cerr
            << "[fva delta-scale " << function_count << "] delta="
            << static_cast<unsigned>(warm_delta.result.result_materialization)
            << " bypass="
            << katana::analysis::persistent_analysis_bypass_reason_name(
                   warm_delta.work.bypass_reason)
            << " avoided=" << warm_delta.work.avoided_full_edge_prepass()
            << " entries=" << warm_delta.work.program_delta_entries_visited
            << " materialized=" << warm_delta.work.final_materialized_blocks
            << '/' << warm_delta.work.final_materialized_functions
            << " incremental="
            << warm_delta.incremental.completed_without_fallback()
            << " terminal_bypass="
            << katana::analysis::persistent_analysis_bypass_reason_name(
                   warm_terminal.work.bypass_reason)
            << " terminal_entries="
            << warm_terminal.work.program_delta_entries_visited
            << " terminal_avoided="
            << warm_terminal.work.avoided_full_edge_prepass()
            << " terminal_materialized="
            << warm_terminal.work.final_materialized_functions
            << " equal=" << warm_equals_fresh
            << " candidate=" << late_candidate_observed << '\n'
            << std::flush;
    }
    require(
        delta_contract,
        "Spaeter Candidate-Seed war nicht delta-lokal, fallbackfrei oder "
        "semantisch gleich zur Fresh-Referenz");

    return {function_count,
            fresh.blocks,
            warm_delta.work,
            warm_terminal.work,
            fresh.work,
            warm_delta.incremental,
            warm_equals_fresh,
            late_candidate_observed};
}

[[nodiscard]] FvaDeltaScaleGate run_fva_delta_scale_gate(
    const std::span<const std::uint8_t> semantic_bytes) {
    constexpr std::size_t n = 16u;
    constexpr std::size_t eight_n = n * 8u;
    FvaDeltaScaleGate gate{
        run_fva_delta_scale_sample(semantic_bytes, n),
        run_fva_delta_scale_sample(semantic_bytes, eight_n)};
    const auto audit_field_is_delta_local =
        [&](std::size_t PersistentWorkLedger::* const field) {
            return gate.n.warm_work.*field > 0u &&
                   gate.eight_n.warm_work.*field ==
                       gate.n.warm_work.*field &&
                   gate.eight_n.fresh_work.*field >
                       gate.n.fresh_work.*field;
        };
    const auto full_edge_prepass_is_zero =
        [](const PersistentWorkLedger& work) {
            return work.function_edge_full_scans == 0u &&
                   work.function_edge_full_sorts == 0u &&
                   work.candidate_call_edge_full_scans == 0u &&
                   work.candidate_call_edge_full_sorts == 0u &&
                   work.candidate_tail_edge_full_scans == 0u &&
                   work.candidate_tail_edge_full_sorts == 0u;
        };
    require(
        gate.eight_n.function_count == gate.n.function_count * 8u &&
            gate.eight_n.block_count > gate.n.block_count &&
            gate.eight_n.fresh_work.program_graph_blocks_built >
                gate.n.fresh_work.program_graph_blocks_built &&
            audit_field_is_delta_local(
                &PersistentWorkLedger::inventory_topology_entries_visited) &&
            audit_field_is_delta_local(
                &PersistentWorkLedger::abi_contract_entries_visited) &&
            audit_field_is_delta_local(
                &PersistentWorkLedger::summary_candidate_entries_visited) &&
            audit_field_is_delta_local(
                &PersistentWorkLedger::resolution_preparation_entries_visited) &&
            full_edge_prepass_is_zero(gate.n.warm_work) &&
            full_edge_prepass_is_zero(gate.eight_n.warm_work) &&
            gate.eight_n.warm_work == gate.n.warm_work &&
            gate.eight_n.warm_incremental.dirty_sccs ==
                gate.n.warm_incremental.dirty_sccs &&
            gate.eight_n.warm_incremental.dirty_functions ==
                gate.n.warm_incremental.dirty_functions &&
            gate.eight_n.warm_incremental.dirty_inventory_sinks ==
                gate.n.warm_incremental.dirty_inventory_sinks,
        "N-vs-8N-O(delta)-Gate skaliert mit dem logischen Gesamtbestand "
        "oder besitzt keine echte groessere Fresh-Referenz");
    return gate;
}

void append_fva_delta_scale_sample_json(
    std::ostream& output,
    const FvaDeltaScaleSample& sample) {
    output << "{\"function_count\":" << sample.function_count
           << ",\"block_count\":" << sample.block_count
           << ",\"warm_equals_fresh\":"
           << (sample.warm_equals_fresh ? "true" : "false")
           << ",\"late_candidate_observed\":"
           << (sample.late_candidate_observed ? "true" : "false")
           << ",\"dirty_sccs\":"
           << sample.warm_incremental.dirty_sccs
           << ",\"dirty_functions\":"
           << sample.warm_incremental.dirty_functions
           << ",\"dirty_inventory_sinks\":"
           << sample.warm_incremental.dirty_inventory_sinks
           << ",\"full_cpu_recompute_fallbacks\":"
           << sample.warm_incremental.full_cpu_recompute_fallbacks
           << ",\"warm_work\":";
    append_persistent_work_json(output, sample.warm_work);
    output << ",\"terminal_presentation_work\":";
    append_persistent_work_json(
        output, sample.terminal_presentation_work);
    output << ",\"fresh_work\":";
    append_persistent_work_json(output, sample.fresh_work);
    output << '}';
}

struct CfaRoundLedger final {
    std::size_t iteration = 0u;
    SeedRoundLedger seeds;
    katana::analysis::PersistentAnalysisBypassReason cfa_bypass_reason =
        katana::analysis::PersistentAnalysisBypassReason::None;
    CfaPhysicalWorkLedger cfa_work;
    PersistentWorkLedger function_value_work;
    IncrementalEpochLedger function_value_incremental;
    std::size_t function_value_invocations = 0u;

    bool operator==(const CfaRoundLedger&) const = default;
};

struct CfaRoundCollection final {
    std::vector<CfaRoundLedger> rounds;
    CfaPhysicalWorkLedger terminal_presentation_work;
    PersistentWorkLedger terminal_function_value_presentation_work;
};

class CfaRoundCollector final {
  public:
    void observe(
        const katana::analysis::ControlFlowAnalysisProgress& progress) {
        const auto cumulative = cfa_physical_work_ledger(progress);
        if (progress.phase == "iteration-start") {
            finish_open_round(
                cumulative,
                progress.persistent_analysis_bypass_reason);
            current_.emplace();
            current_->iteration = progress.iteration;
            current_->seeds = seed_round_ledger(progress);
            round_begin_ = cumulative;
            return;
        }
        if (current_.has_value() && progress.function_value_active &&
            progress.phase.starts_with("function-values-complete-f")) {
            current_->function_value_work.add(
                persistent_work_ledger(progress));
            current_->function_value_incremental.add(
                incremental_epoch_ledger(progress));
            ++current_->function_value_invocations;
        }
        if (progress.phase ==
            "analysis-terminal-materialization-start") {
            require(!terminal_started_ && !terminal_observed_,
                    "CFA-O(delta)-Gate beobachtete einen doppelten "
                    "Terminalstart");
            finish_open_round(
                cumulative,
                progress.persistent_analysis_bypass_reason);
            terminal_begin_ = cumulative;
            terminal_started_ = true;
            return;
        }
        if (terminal_started_ && progress.function_value_active &&
            progress.phase == "function-values-terminal-materialized") {
            terminal_function_value_presentation_work_.add(
                persistent_work_ledger(progress));
            ++terminal_function_value_presentations_;
            return;
        }
        if (progress.phase == "analysis-terminal-materialized") {
            require(terminal_started_ && !terminal_observed_ &&
                        !current_.has_value(),
                    "CFA-O(delta)-Gate beobachtete ein ungebundenes "
                    "Terminalende");
            terminal_presentation_work_ = subtract_cfa_work(
                cumulative, terminal_begin_);
            terminal_observed_ = true;
            return;
        }
    }

    [[nodiscard]] CfaRoundCollection finish() {
        require(terminal_started_ && terminal_observed_ &&
                    terminal_function_value_presentations_ == 1u &&
                    !current_.has_value(),
                "CFA-O(delta)-Gate verlor den terminalen Fixpunkt");
        return {std::move(rounds_),
                terminal_presentation_work_,
                terminal_function_value_presentation_work_};
    }

  private:
    void finish_open_round(
        const CfaPhysicalWorkLedger& cumulative,
        const katana::analysis::PersistentAnalysisBypassReason
            bypass_reason) {
        if (!current_.has_value()) return;
        current_->cfa_bypass_reason = bypass_reason;
        current_->cfa_work = subtract_cfa_work(
            cumulative, round_begin_);
        rounds_.push_back(std::move(*current_));
        current_.reset();
    }

    std::optional<CfaRoundLedger> current_;
    CfaPhysicalWorkLedger round_begin_;
    CfaPhysicalWorkLedger terminal_begin_;
    CfaPhysicalWorkLedger terminal_presentation_work_;
    PersistentWorkLedger terminal_function_value_presentation_work_;
    std::vector<CfaRoundLedger> rounds_;
    std::size_t terminal_function_value_presentations_ = 0u;
    bool terminal_started_ = false;
    bool terminal_observed_ = false;
};

struct CfaSemanticComparison final {
    bool equal = false;
    bool synthetic_fresh_hint_normalization_verified = false;
};

[[nodiscard]] CfaSemanticComparison same_cfa_semantics(
    const katana::analysis::ControlFlowAnalysisResult& left,
    const katana::analysis::ControlFlowAnalysisResult& right) {
    const auto same_sequence = [](const auto& lhs,
                                  const auto& rhs,
                                  const auto& same_element) {
        return lhs.size() == rhs.size() &&
               std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                          same_element);
    };
    const auto same_line = [](const auto& lhs, const auto& rhs) {
        const auto& li = lhs.instruction;
        const auto& ri = rhs.instruction;
        return lhs.address == rhs.address && lhs.opcode == rhs.opcode &&
               lhs.is_delay_slot == rhs.is_delay_slot &&
               lhs.target_address == rhs.target_address &&
               li.opcode == ri.opcode && li.kind == ri.kind &&
               li.destination_register == ri.destination_register &&
               li.source_register == ri.source_register &&
               li.branch_register == ri.branch_register &&
               li.immediate == ri.immediate &&
               li.displacement == ri.displacement &&
               li.special_register == ri.special_register &&
               li.control_flow == ri.control_flow &&
               li.has_delay_slot == ri.has_delay_slot &&
               li.is_privileged == ri.is_privileged && li.text == ri.text;
    };
    const auto same_contextual_instruction = [&](const auto& lhs,
                                                 const auto& rhs) {
        return same_line(lhs.line, rhs.line) &&
               lhs.incoming_address == rhs.incoming_address &&
               lhs.delay_slot_owner == rhs.delay_slot_owner &&
               lhs.evidence == rhs.evidence;
    };
    const auto same_range = [](const auto& lhs, const auto& rhs) {
        return lhs.start_address == rhs.start_address &&
               lhs.size == rhs.size && lhs.kind == rhs.kind;
    };
    const auto same_function = [](const auto& lhs, const auto& rhs) {
        return lhs.address == rhs.address &&
               lhs.confidence == rhs.confidence &&
               lhs.evidence == rhs.evidence &&
               lhs.origins == rhs.origins && lhs.size == rhs.size;
    };
    const auto same_conflict = [](const auto& lhs, const auto& rhs) {
        return lhs.address == rhs.address && lhs.size == rhs.size &&
               lhs.kind == rhs.kind;
    };
    const auto same_analysis_diagnostic = [](const auto& lhs,
                                             const auto& rhs) {
        return lhs.address == rhs.address && lhs.opcode == rhs.opcode &&
               lhs.kind == rhs.kind && lhs.reason == rhs.reason &&
               lhs.evidence == rhs.evidence;
    };
    const auto same_runtime_patch = [](const auto& lhs, const auto& rhs) {
        return lhs.store_instruction_address ==
                   rhs.store_instruction_address &&
               lhs.slot_address == rhs.slot_address &&
               lhs.live_value == rhs.live_value &&
               lhs.target_address == rhs.target_address;
    };
    const auto same_runtime_mutable_range = [](const auto& lhs,
                                               const auto& rhs) {
        return lhs.store_instruction_address ==
                   rhs.store_instruction_address &&
               lhs.load_instruction_address ==
                   rhs.load_instruction_address &&
               lhs.slot_address == rhs.slot_address &&
               lhs.size == rhs.size;
    };
    const auto same_runtime_copy = [&](const auto& lhs, const auto& rhs) {
        return lhs.setup_address == rhs.setup_address &&
               lhs.loop_address == rhs.loop_address &&
               lhs.source_begin == rhs.source_begin &&
               lhs.source_end_inclusive == rhs.source_end_inclusive &&
               lhs.source_byte_count == rhs.source_byte_count &&
               lhs.destination_vbr_delta == rhs.destination_vbr_delta &&
               same_sequence(lhs.patch_candidates,
                             rhs.patch_candidates,
                             same_runtime_patch) &&
               same_sequence(lhs.mutable_ranges,
                             rhs.mutable_ranges,
                             same_runtime_mutable_range) &&
               lhs.mutable_range_analysis_complete ==
                   rhs.mutable_range_analysis_complete &&
               lhs.evidence == rhs.evidence &&
               lhs.aot_candidates_only == rhs.aot_candidates_only &&
               lhs.reason == rhs.reason;
    };
    const auto same_indirect = [](const auto& lhs, const auto& rhs) {
        return lhs.instruction_address == rhs.instruction_address &&
               lhs.kind == rhs.kind &&
               lhs.register_index == rhs.register_index &&
               lhs.status == rhs.status && lhs.evidence == rhs.evidence &&
               lhs.origin_class == rhs.origin_class &&
               lhs.evidence_origins == rhs.evidence_origins &&
               lhs.target == rhs.target && lhs.reason == rhs.reason &&
               lhs.targets == rhs.targets &&
               lhs.evidence_call_sites == rhs.evidence_call_sites &&
               lhs.evidence_callees == rhs.evidence_callees &&
               lhs.value_source == rhs.value_source &&
               lhs.definition_sites == rhs.definition_sites &&
               lhs.definition_complete == rhs.definition_complete &&
               lhs.preceding_call == rhs.preceding_call &&
               lhs.instruction_kind == rhs.instruction_kind &&
               lhs.analysis_candidates == rhs.analysis_candidates;
    };
    const auto same_static_continuation = [](const auto& lhs,
                                             const auto& rhs) {
        return lhs.instruction_address == rhs.instruction_address &&
               lhs.register_index == rhs.register_index &&
               lhs.target_address == rhs.target_address &&
               lhs.evidence == rhs.evidence &&
               lhs.evidence_origins == rhs.evidence_origins &&
               lhs.reason == rhs.reason &&
               lhs.value_source == rhs.value_source;
    };
    const auto same_jump_table_entry = [](const auto& lhs,
                                          const auto& rhs) {
        return lhs.index == rhs.index &&
               lhs.entry_address == rhs.entry_address &&
               lhs.target == rhs.target && lhs.accepted == rhs.accepted &&
               lhs.reason == rhs.reason;
    };
    const auto same_jump_table = [&](const auto& lhs, const auto& rhs) {
        return lhs.dispatch_address == rhs.dispatch_address &&
               lhs.table_address == rhs.table_address &&
               lhs.target_base == rhs.target_base &&
               lhs.requested_entries == rhs.requested_entries &&
               lhs.dispatch_kind == rhs.dispatch_kind &&
               lhs.encoding == rhs.encoding &&
               lhs.resolved == rhs.resolved &&
               lhs.aot_candidates_only == rhs.aot_candidates_only &&
               lhs.candidate_scan_truncated ==
                   rhs.candidate_scan_truncated &&
               lhs.evidence == rhs.evidence &&
               same_sequence(lhs.entries,
                             rhs.entries,
                             same_jump_table_entry) &&
               lhs.reason == rhs.reason;
    };
    const auto same_site = [](const auto& lhs, const auto& rhs) {
        return lhs.instruction_address == rhs.instruction_address &&
               lhs.kind == rhs.kind && lhs.evidence == rhs.evidence &&
               lhs.origin_class == rhs.origin_class &&
               lhs.evidence_origins == rhs.evidence_origins &&
               lhs.targets == rhs.targets &&
               lhs.evidence_call_sites == rhs.evidence_call_sites &&
               lhs.evidence_callees == rhs.evidence_callees;
    };
    const auto same_guarded_entry = [](const auto& lhs, const auto& rhs) {
        return lhs.guest_address == rhs.guest_address &&
               lhs.shared_body_address == rhs.shared_body_address &&
               lhs.evidence == rhs.evidence &&
               lhs.origins == rhs.origins &&
               lhs.source_sites == rhs.source_sites &&
               lhs.source_objects == rhs.source_objects &&
               lhs.source_identity == rhs.source_identity &&
               lhs.source_byte_offset == rhs.source_byte_offset &&
               lhs.entry_byte_extent == rhs.entry_byte_extent &&
               lhs.entry_byte_identity == rhs.entry_byte_identity;
    };
    const auto same_guarded_rejection = [](const auto& lhs,
                                           const auto& rhs) {
        return lhs.guest_address == rhs.guest_address &&
               lhs.resolved_address == rhs.resolved_address &&
               lhs.reason == rhs.reason && lhs.evidence == rhs.evidence &&
               lhs.origins == rhs.origins &&
               lhs.source_sites == rhs.source_sites &&
               lhs.source_objects == rhs.source_objects;
    };
    const auto same_directive_diagnostic = [](const auto& lhs,
                                              const auto& rhs) {
        return lhs.line == rhs.line && lhs.address == rhs.address &&
               lhs.status == rhs.status && lhs.reason == rhs.reason;
    };
    const auto same_symbolic_address = [](const auto& lhs,
                                          const auto& rhs) {
        return lhs.address == rhs.address &&
               lhs.symbol_address == rhs.symbol_address &&
               lhs.offset == rhs.offset && lhs.name == rhs.name &&
               lhs.kind == rhs.kind && lhs.binding == rhs.binding &&
               lhs.exact == rhs.exact;
    };
    const auto same_seed_cause = [](const auto& lhs, const auto& rhs) {
        return lhs == rhs;
    };
    const auto same_seed_fact = [&](const auto& lhs, const auto& rhs) {
        return lhs.target_address == rhs.target_address &&
               lhs.origins == rhs.origins && lhs.proven == rhs.proven &&
               lhs.evidence == rhs.evidence &&
               lhs.function_size == rhs.function_size &&
               same_sequence(lhs.causes, rhs.causes, same_seed_cause);
    };
    const auto no_analysis_user_hint = [](const auto& analysis) {
        const auto lacks_hint = [](const auto& origins) {
            return std::find(origins.begin(), origins.end(),
                             katana::analysis::AnalysisEvidenceOrigin::
                                 UserHint) == origins.end();
        };
        return std::all_of(
                   analysis.indirect_control_flow.begin(),
                   analysis.indirect_control_flow.end(),
                   [&](const auto& value) {
                       return lacks_hint(value.evidence_origins);
                   }) &&
               std::all_of(
                   analysis.static_return_continuations.begin(),
                   analysis.static_return_continuations.end(),
                   [&](const auto& value) {
                       return lacks_hint(value.evidence_origins);
                   }) &&
               std::all_of(
                   analysis.resolved_edges.begin(),
                   analysis.resolved_edges.end(),
                   [&](const auto& value) {
                       return lacks_hint(value.evidence_origins);
                   }) &&
               std::all_of(analysis.sites.begin(),
                           analysis.sites.end(),
                           [&](const auto& value) {
                               return lacks_hint(value.evidence_origins);
                           });
    };

    auto left_seed_facts = left.seed_facts;
    auto right_seed_facts = right.seed_facts;
    auto left_contexts = left.recursive.contextual_instructions;
    auto right_contexts = right.recursive.contextual_instructions;
    auto left_functions = left.recursive.functions;
    auto right_functions = right.recursive.functions;
    auto left_seed_contract = left.recursive.seed_contract;
    auto right_seed_contract = right.recursive.seed_contract;
    auto left_directives = left.directive_diagnostics;
    auto right_directives = right.directive_diagnostics;
    const auto evidence_strings = [](const auto& analysis) {
        std::vector<std::string> result;
        result.reserve(analysis.evidence_ids.size());
        for (std::size_t index = 1u;
             index <= analysis.evidence_ids.size(); ++index) {
            result.emplace_back(analysis.evidence_ids.resolve(
                static_cast<katana::analysis::EvidenceId>(index)));
        }
        return result;
    };
    auto left_evidence_strings = evidence_strings(left);
    auto right_evidence_strings = evidence_strings(right);
    const auto user_hint_origin =
        katana::analysis::FunctionOrigin::UserHint;
    const auto function_directive =
        katana::analysis::ControlFlowAnalysisResult::SeedCauseKind::
            FunctionDirective;
    const auto count_origin = [user_hint_origin](const auto& values) {
        return static_cast<std::size_t>(std::count(
            values.begin(), values.end(), user_hint_origin));
    };
    const auto left_seed_hint_count = std::accumulate(
        left_seed_facts.begin(), left_seed_facts.end(), std::size_t{0u},
        [&](const auto count, const auto& fact) {
            return count + count_origin(fact.origins);
        });
    const auto right_seed_hint_count = std::accumulate(
        right_seed_facts.begin(), right_seed_facts.end(), std::size_t{0u},
        [&](const auto count, const auto& fact) {
            return count + count_origin(fact.origins);
        });
    const auto count_function_directives =
        [function_directive](const auto& facts) {
            std::size_t count = 0u;
            for (const auto& fact : facts)
                count += static_cast<std::size_t>(std::count_if(
                    fact.causes.begin(), fact.causes.end(),
                    [function_directive](const auto& cause) {
                        return cause.kind == function_directive;
                    }));
            return count;
        };
    const auto left_function_hint_count = std::accumulate(
        left_functions.begin(), left_functions.end(), std::size_t{0u},
        [&](const auto count, const auto& function) {
            return count + count_origin(function.origins);
        });
    const auto right_function_hint_count = std::accumulate(
        right_functions.begin(), right_functions.end(), std::size_t{0u},
        [&](const auto count, const auto& function) {
            return count + count_origin(function.origins);
        });
    const auto left_contract_hint_count = std::accumulate(
        left_seed_contract.begin(), left_seed_contract.end(),
        std::size_t{0u},
        [&](const auto count, const auto& contract) {
            return count + count_origin(contract.function_origins);
        });
    const auto right_contract_hint_count = std::accumulate(
        right_seed_contract.begin(), right_seed_contract.end(),
        std::size_t{0u},
        [&](const auto count, const auto& contract) {
            return count + count_origin(contract.function_origins);
        });
    const auto left_fact = std::find_if(
        left_seed_facts.begin(), left_seed_facts.end(),
        [](const auto& fact) { return fact.target_address == 0x180u; });
    const auto right_fact = std::find_if(
        right_seed_facts.begin(), right_seed_facts.end(),
        [](const auto& fact) { return fact.target_address == 0x180u; });
    const auto left_function = std::find_if(
        left_functions.begin(), left_functions.end(),
        [](const auto& function) { return function.address == 0x180u; });
    const auto right_function = std::find_if(
        right_functions.begin(), right_functions.end(),
        [](const auto& function) { return function.address == 0x180u; });
    const auto left_contract = std::find_if(
        left_seed_contract.begin(), left_seed_contract.end(),
        [](const auto& contract) { return contract.address == 0x180u; });
    const auto right_contract = std::find_if(
        right_seed_contract.begin(), right_seed_contract.end(),
        [](const auto& contract) { return contract.address == 0x180u; });
    const auto expected_directive_cause =
        [function_directive](const auto& cause) {
            return cause.kind == function_directive &&
                   cause.source_address == 0x180u &&
                   cause.source_object == 1u &&
                   cause.owner_address == 0x180u &&
                   cause.evidence_call_sites.empty() &&
                   cause.evidence_callees.empty();
        };
    const auto expected_directive_cause_count =
        right_fact == right_seed_facts.end()
            ? std::size_t{0u}
            : static_cast<std::size_t>(std::count_if(
                  right_fact->causes.begin(),
                  right_fact->causes.end(),
                  expected_directive_cause));
    constexpr auto discovered_evidence =
        katana::analysis::ControlFlowEvidence::RuntimeOnly;
    constexpr auto synthetic_hint_evidence =
        katana::analysis::ControlFlowEvidence::HintCandidate;
    constexpr std::array synthetic_hint_context_addresses{
        0x180u, 0x182u, 0x184u, 0x186u, 0x188u};
    std::size_t normalized_context_count = 0u;
    bool context_normalization_verified =
        left_contexts.size() == right_contexts.size();
    if (context_normalization_verified) {
        for (std::size_t index = 0u; index < left_contexts.size(); ++index) {
            const auto& left_context = left_contexts[index];
            const auto& right_context = right_contexts[index];
            if (!same_line(left_context.line, right_context.line) ||
                left_context.incoming_address !=
                    right_context.incoming_address ||
                left_context.delay_slot_owner !=
                    right_context.delay_slot_owner) {
                context_normalization_verified = false;
                break;
            }
            if (left_context.evidence == right_context.evidence) continue;
            const auto expected_address = std::find(
                synthetic_hint_context_addresses.begin(),
                synthetic_hint_context_addresses.end(),
                left_context.line.address) !=
                synthetic_hint_context_addresses.end();
            if (!expected_address ||
                left_context.evidence != discovered_evidence ||
                right_context.evidence != synthetic_hint_evidence) {
                context_normalization_verified = false;
                break;
            }
            ++normalized_context_count;
        }
    }
    context_normalization_verified =
        context_normalization_verified &&
        normalized_context_count == synthetic_hint_context_addresses.size();
    const auto normalization_verified =
        left_seed_hint_count == 0u &&
        count_function_directives(left_seed_facts) == 0u &&
        left_function_hint_count == 0u &&
        left_contract_hint_count == 0u && left_directives.empty() &&
        right_seed_hint_count == 1u &&
        count_function_directives(right_seed_facts) == 1u &&
        right_function_hint_count == 1u &&
        right_contract_hint_count == 1u &&
        context_normalization_verified &&
        left_fact != left_seed_facts.end() &&
        right_fact != right_seed_facts.end() &&
        left_fact->evidence == discovered_evidence &&
        right_fact->evidence == synthetic_hint_evidence &&
        count_origin(right_fact->origins) == 1u &&
        expected_directive_cause_count == 1u &&
        left_function != left_functions.end() &&
        right_function != right_functions.end() &&
        left_function->evidence == discovered_evidence &&
        right_function->evidence == synthetic_hint_evidence &&
        count_origin(right_function->origins) == 1u &&
        left_contract != left_seed_contract.end() &&
        right_contract != right_seed_contract.end() &&
        left_contract->decode_evidences ==
            std::vector<katana::analysis::ControlFlowEvidence>{
                discovered_evidence} &&
        right_contract->decode_evidences ==
            std::vector<katana::analysis::ControlFlowEvidence>{
                synthetic_hint_evidence} &&
        count_origin(right_contract->function_origins) == 1u &&
        right_directives.size() == 1u &&
        right_directives.front().line == 1u &&
        right_directives.front().address == 0x180u &&
        right_directives.front().status ==
            katana::analysis::AnalysisDirectiveDiagnosticStatus::Accepted &&
        right_directives.front().reason == "function-seed" &&
        std::count(left_evidence_strings.begin(),
                   left_evidence_strings.end(),
                   "function-seed") == 0 &&
        std::count(right_evidence_strings.begin(),
                   right_evidence_strings.end(),
                   "function-seed") == 1 &&
        no_analysis_user_hint(left) && no_analysis_user_hint(right);
    if (!normalization_verified) return {false, false};

    right_fact->origins.erase(
        std::find(right_fact->origins.begin(),
                  right_fact->origins.end(), user_hint_origin));
    right_fact->causes.erase(std::find_if(
        right_fact->causes.begin(), right_fact->causes.end(),
        expected_directive_cause));
    right_fact->evidence = discovered_evidence;
    right_function->origins.erase(
        std::find(right_function->origins.begin(),
                  right_function->origins.end(), user_hint_origin));
    right_function->evidence = discovered_evidence;
    right_contract->function_origins.erase(
        std::find(right_contract->function_origins.begin(),
                  right_contract->function_origins.end(),
                  user_hint_origin));
    right_contract->decode_evidences = {discovered_evidence};
    for (std::size_t index = 0u; index < right_contexts.size(); ++index) {
        if (left_contexts[index].evidence != right_contexts[index].evidence)
            right_contexts[index].evidence = discovered_evidence;
    }
    right_directives.clear();
    right_evidence_strings.erase(std::find(
        right_evidence_strings.begin(),
        right_evidence_strings.end(), "function-seed"));

    const auto same_instruction_arena = [&] {
        if (left.instruction_arena == nullptr ||
            right.instruction_arena == nullptr)
            return left.instruction_arena == right.instruction_arena;
        return same_sequence(left.instruction_arena->instructions(),
                             right.instruction_arena->instructions(),
                             same_line);
    };
    auto left_walk = left.guarded_code_inventory_walk;
    auto right_walk = right.guarded_code_inventory_walk;
    left_walk.forwarded_store_evaluation_cache_hits = 0u;
    left_walk.forwarded_store_evaluation_cache_misses = 0u;
    right_walk.forwarded_store_evaluation_cache_hits = 0u;
    right_walk.forwarded_store_evaluation_cache_misses = 0u;
    const auto equal =
           same_sequence(left.recursive.instructions,
                         right.recursive.instructions,
                         same_line) &&
           same_sequence(left_contexts,
                         right_contexts,
                         same_contextual_instruction) &&
           left.recursive.proven_instruction_addresses ==
               right.recursive.proven_instruction_addresses &&
           left.recursive.guarded_candidate_instruction_addresses ==
               right.recursive.guarded_candidate_instruction_addresses &&
           same_sequence(left.recursive.ranges,
                         right.recursive.ranges,
                         same_range) &&
           same_sequence(left.recursive.unreachable_code,
                         right.recursive.unreachable_code,
                         same_range) &&
           same_sequence(left_functions, right_functions, same_function) &&
           same_sequence(left.recursive.conflicts,
                         right.recursive.conflicts,
                         same_conflict) &&
           same_sequence(left.recursive.diagnostics,
                         right.recursive.diagnostics,
                         same_analysis_diagnostic) &&
           left.recursive.limit == right.recursive.limit &&
           left.recursive.retained_baseline_contract_version ==
               right.recursive.retained_baseline_contract_version &&
           left.recursive.source_image_identity ==
               right.recursive.source_image_identity &&
           left.recursive.source_image_revision ==
               right.recursive.source_image_revision &&
           left_seed_contract == right_seed_contract &&
           left.recursive.baseline_status ==
               right.recursive.baseline_status &&
           same_sequence(left.runtime_code_copies.copies,
                         right.runtime_code_copies.copies,
                         same_runtime_copy) &&
           same_sequence(left.indirect_control_flow,
                         right.indirect_control_flow,
                         same_indirect) &&
           same_sequence(left.static_return_continuations,
                         right.static_return_continuations,
                         same_static_continuation) &&
           same_sequence(left.jump_tables,
                         right.jump_tables,
                         same_jump_table) &&
           left.function_value_summaries ==
               right.function_value_summaries &&
           left.resolved_edges == right.resolved_edges &&
           same_sequence(left.sites, right.sites, same_site) &&
           same_sequence(left.guarded_aot_entries,
                         right.guarded_aot_entries,
                         same_guarded_entry) &&
           same_sequence(left.guarded_aot_entry_rejections,
                         right.guarded_aot_entry_rejections,
                         same_guarded_rejection) &&
           same_instruction_arena() &&
           left.block_spans == right.block_spans &&
           left_evidence_strings == right_evidence_strings &&
           left.function_scc_count == right.function_scc_count &&
           left.function_iteration_budget ==
               right.function_iteration_budget &&
           left_walk == right_walk &&
           left.raw_stored_code_inventory_candidates ==
               right.raw_stored_code_inventory_candidates &&
           left.raw_stored_code_inventory_budget ==
               right.raw_stored_code_inventory_budget &&
           left.guarded_code_inventory_candidates ==
               right.guarded_code_inventory_candidates &&
           left.guarded_code_inventory_budget ==
               right.guarded_code_inventory_budget &&
           left.guarded_code_shape_validation_work ==
               right.guarded_code_shape_validation_work &&
           left.guarded_code_shape_validation_work_budget ==
               right.guarded_code_shape_validation_work_budget &&
           left.guarded_code_shape_budget_exceeded_candidates ==
               right.guarded_code_shape_budget_exceeded_candidates &&
           left.function_budget_exhausted ==
               right.function_budget_exhausted &&
           left.termination_reason == right.termination_reason &&
           left.progress_callback_failed == right.progress_callback_failed &&
           left.raw_stored_code_inventory_truncated ==
               right.raw_stored_code_inventory_truncated &&
           left.guarded_code_inventory_candidate_budget_exhausted ==
               right.guarded_code_inventory_candidate_budget_exhausted &&
           left.candidate_inventory_truncated ==
               right.candidate_inventory_truncated &&
           left.returned_table_scan_truncated ==
               right.returned_table_scan_truncated &&
           same_sequence(left_directives,
                         right_directives,
                         same_directive_diagnostic) &&
           same_sequence(left.symbolic_addresses,
                         right.symbolic_addresses,
                         same_symbolic_address) &&
           same_sequence(left_seed_facts,
                         right_seed_facts,
                         same_seed_fact) &&
           left.persistent_analysis_bypass_reason ==
               right.persistent_analysis_bypass_reason &&
           katana::analysis::guarded_aot_inventory_complete(left) ==
               katana::analysis::guarded_aot_inventory_complete(right);
    if (!equal) {
        const std::array components{
            std::pair{std::string_view{"recursive.instructions"},
                      same_sequence(left.recursive.instructions,
                                    right.recursive.instructions,
                                    same_line)},
            std::pair{std::string_view{"recursive.contexts"},
                      same_sequence(left.recursive.contextual_instructions,
                                    right.recursive.contextual_instructions,
                                    same_contextual_instruction)},
            std::pair{std::string_view{"recursive.proven"},
                      left.recursive.proven_instruction_addresses ==
                          right.recursive.proven_instruction_addresses},
            std::pair{std::string_view{"recursive.guarded"},
                      left.recursive.guarded_candidate_instruction_addresses ==
                          right.recursive.guarded_candidate_instruction_addresses},
            std::pair{std::string_view{"recursive.ranges"},
                      same_sequence(left.recursive.ranges,
                                    right.recursive.ranges, same_range)},
            std::pair{std::string_view{"recursive.unreachable"},
                      same_sequence(left.recursive.unreachable_code,
                                    right.recursive.unreachable_code,
                                    same_range)},
            std::pair{std::string_view{"recursive.functions"},
                      same_sequence(left_functions, right_functions,
                                    same_function)},
            std::pair{std::string_view{"recursive.conflicts"},
                      same_sequence(left.recursive.conflicts,
                                    right.recursive.conflicts,
                                    same_conflict)},
            std::pair{std::string_view{"recursive.diagnostics"},
                      same_sequence(left.recursive.diagnostics,
                                    right.recursive.diagnostics,
                                    same_analysis_diagnostic)},
            std::pair{std::string_view{"recursive.limit"},
                      left.recursive.limit == right.recursive.limit},
            std::pair{std::string_view{"recursive.contract-version"},
                      left.recursive.retained_baseline_contract_version ==
                          right.recursive.retained_baseline_contract_version},
            std::pair{std::string_view{"recursive.image-identity"},
                      left.recursive.source_image_identity ==
                          right.recursive.source_image_identity},
            std::pair{std::string_view{"recursive.image-revision"},
                      left.recursive.source_image_revision ==
                          right.recursive.source_image_revision},
            std::pair{std::string_view{"recursive.seed-contract"},
                      left_seed_contract == right_seed_contract},
            std::pair{std::string_view{"recursive.baseline-status"},
                      left.recursive.baseline_status ==
                          right.recursive.baseline_status},
            std::pair{std::string_view{"runtime-copies"},
                      same_sequence(left.runtime_code_copies.copies,
                                    right.runtime_code_copies.copies,
                                    same_runtime_copy)},
            std::pair{std::string_view{"indirect"},
                      same_sequence(left.indirect_control_flow,
                                    right.indirect_control_flow,
                                    same_indirect)},
            std::pair{std::string_view{"continuations"},
                      same_sequence(left.static_return_continuations,
                                    right.static_return_continuations,
                                    same_static_continuation)},
            std::pair{std::string_view{"jump-tables"},
                      same_sequence(left.jump_tables, right.jump_tables,
                                    same_jump_table)},
            std::pair{std::string_view{"summaries"},
                      left.function_value_summaries ==
                          right.function_value_summaries},
            std::pair{std::string_view{"resolved-edges"},
                      left.resolved_edges == right.resolved_edges},
            std::pair{std::string_view{"sites"},
                      same_sequence(left.sites, right.sites, same_site)},
            std::pair{std::string_view{"guarded-entries"},
                      same_sequence(left.guarded_aot_entries,
                                    right.guarded_aot_entries,
                                    same_guarded_entry)},
            std::pair{std::string_view{"guarded-rejections"},
                      same_sequence(left.guarded_aot_entry_rejections,
                                    right.guarded_aot_entry_rejections,
                                    same_guarded_rejection)},
            std::pair{std::string_view{"instruction-arena"},
                      same_instruction_arena()},
            std::pair{std::string_view{"block-spans"},
                      left.block_spans == right.block_spans},
            std::pair{std::string_view{"evidence"},
                      left_evidence_strings == right_evidence_strings},
            std::pair{std::string_view{"scc-count"},
                      left.function_scc_count == right.function_scc_count},
            std::pair{std::string_view{"iteration-budget"},
                      left.function_iteration_budget ==
                          right.function_iteration_budget},
            std::pair{std::string_view{"walk"}, left_walk == right_walk},
            std::pair{std::string_view{"raw-candidates"},
                      left.raw_stored_code_inventory_candidates ==
                          right.raw_stored_code_inventory_candidates},
            std::pair{std::string_view{"raw-budget"},
                      left.raw_stored_code_inventory_budget ==
                          right.raw_stored_code_inventory_budget},
            std::pair{std::string_view{"guarded-candidates"},
                      left.guarded_code_inventory_candidates ==
                          right.guarded_code_inventory_candidates},
            std::pair{std::string_view{"guarded-budget"},
                      left.guarded_code_inventory_budget ==
                          right.guarded_code_inventory_budget},
            std::pair{std::string_view{"shape-work"},
                      left.guarded_code_shape_validation_work ==
                          right.guarded_code_shape_validation_work},
            std::pair{std::string_view{"shape-budget"},
                      left.guarded_code_shape_validation_work_budget ==
                          right.guarded_code_shape_validation_work_budget},
            std::pair{std::string_view{"shape-exceeded"},
                      left.guarded_code_shape_budget_exceeded_candidates ==
                          right.guarded_code_shape_budget_exceeded_candidates},
            std::pair{std::string_view{"function-budget"},
                      left.function_budget_exhausted ==
                          right.function_budget_exhausted},
            std::pair{std::string_view{"termination"},
                      left.termination_reason == right.termination_reason},
            std::pair{std::string_view{"progress-callback"},
                      left.progress_callback_failed ==
                          right.progress_callback_failed},
            std::pair{std::string_view{"raw-truncation"},
                      left.raw_stored_code_inventory_truncated ==
                          right.raw_stored_code_inventory_truncated},
            std::pair{std::string_view{"guarded-budget-exhaustion"},
                      left.guarded_code_inventory_candidate_budget_exhausted ==
                          right.guarded_code_inventory_candidate_budget_exhausted},
            std::pair{std::string_view{"candidate-truncation"},
                      left.candidate_inventory_truncated ==
                          right.candidate_inventory_truncated},
            std::pair{std::string_view{"table-truncation"},
                      left.returned_table_scan_truncated ==
                          right.returned_table_scan_truncated},
            std::pair{std::string_view{"directives"},
                      same_sequence(left_directives, right_directives,
                                    same_directive_diagnostic)},
            std::pair{std::string_view{"symbols"},
                      same_sequence(left.symbolic_addresses,
                                    right.symbolic_addresses,
                                    same_symbolic_address)},
            std::pair{std::string_view{"seed-facts"},
                      same_sequence(left_seed_facts, right_seed_facts,
                                    same_seed_fact)},
            std::pair{std::string_view{"persistent-bypass"},
                      left.persistent_analysis_bypass_reason ==
                          right.persistent_analysis_bypass_reason},
            std::pair{std::string_view{"inventory-complete"},
                      katana::analysis::guarded_aot_inventory_complete(left) ==
                          katana::analysis::guarded_aot_inventory_complete(right)}};
        std::cerr << "[cfa semantic-component-diff]";
        for (const auto& [name, same] : components)
            if (!same) std::cerr << ' ' << name;
        std::cerr << '\n' << std::flush;
    }
    return {equal, true};
}

[[nodiscard]] katana::io::ExecutableImage make_cfa_delta_scale_image(
    const std::size_t function_count) {
    // A, B and the candidate-discovered C are ordinary CFG functions. C stores
    // the 0x1E0 callback into the scalar destination at 0x1F0; the callback is
    // therefore both a guarded-inventory entry and the fourth decoded function.
    constexpr std::size_t fixture_function_count = 4u;
    constexpr std::size_t fixture_size = 0x200u;
    constexpr std::size_t filler_size = 4u;
    require(function_count >= fixture_function_count,
            "CFA-O(delta)-Fixture besitzt zu wenige Funktionen");
    const auto filler_count = function_count - fixture_function_count;
    std::vector<std::uint8_t> bytes(
        fixture_size + filler_count * filler_size, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    // A owns 65 outgoing stack facts. Only slot zero is a callback.
    put_u16(0x00u, 0xD045u);
    put_u16(0x02u, 0xE201u);
    put_u16(0x04u, 0x61F3u);
    put_u16(0x06u, 0x2102u);
    auto cursor = std::size_t{0x08u};
    for (std::size_t slot = 1u; slot <= 64u; ++slot) {
        static_cast<void>(slot);
        put_u16(cursor, 0x7104u);
        put_u16(cursor + 2u, 0x2122u);
        cursor += 4u;
    }
    put_u16(0x108u, 0xB01Au);
    put_u16(0x10Au, 0xE400u);
    put_u16(0x10Cu, 0x000Bu);
    put_u16(0x10Eu, 0x0009u);
    put_u32(0x118u, 0x1E0u);

    // B loads the indirect target before a basic-block boundary. The target
    // is candidate-only and intentionally absent from the initial decode set.
    put_u16(0x140u, 0xD307u);
    put_u16(0x142u, 0x2742u);
    put_u16(0x144u, 0xA002u);
    put_u16(0x146u, 0x0009u);
    put_u16(0x14Cu, 0x430Bu);
    put_u16(0x14Eu, 0x0009u);
    put_u16(0x150u, 0x000Bu);
    put_u16(0x152u, 0x0009u);
    put_u32(0x160u, 0x180u);

    // C is decoded only after that Candidate seed. It consumes stack slot
    // zero and produces the second, inventory-only callback seed.
    put_u16(0x180u, 0x64F2u);
    put_u16(0x182u, 0xD503u);
    put_u16(0x184u, 0x2542u);
    put_u16(0x186u, 0x000Bu);
    put_u16(0x188u, 0x0009u);
    put_u32(0x190u, 0x1F0u);
    put_u16(0x1E0u, 0x000Bu);
    put_u16(0x1E2u, 0x0009u);

    for (std::size_t index = 0u; index < filler_count; ++index) {
        const auto offset = fixture_size + index * filler_size;
        put_u16(offset, 0x000Bu);
        put_u16(offset + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.add_segment({".kr4978-cfa-delta-scale",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       std::move(bytes),
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-kr4978-cfa-delta-scale"});
    image.add_entry_point(0u);
    for (std::size_t index = 0u; index < filler_count; ++index)
        image.add_entry_point(static_cast<std::uint32_t>(
            fixture_size + index * filler_size));
    return image;
}

[[nodiscard]] bool has_cfa_seed_cause(
    const katana::analysis::ControlFlowAnalysisResult& analysis,
    const std::uint32_t target,
    const katana::analysis::ControlFlowAnalysisResult::SeedCauseKind kind,
    const std::uint32_t source) {
    const auto fact = std::find_if(
        analysis.seed_facts.begin(), analysis.seed_facts.end(),
        [target](const auto& candidate) {
            return candidate.target_address == target;
        });
    return fact != analysis.seed_facts.end() &&
           std::any_of(
               fact->causes.begin(), fact->causes.end(),
               [kind, source](const auto& cause) {
                   return cause.kind == kind &&
                          cause.source_address == source;
               });
}

struct CfaDeltaScaleSample final {
    std::size_t function_count = 0u;
    std::size_t instruction_count = 0u;
    bool warm_equals_fresh = false;
    bool synthetic_fresh_hint_normalization_verified = false;
    bool late_candidate_observed = false;
    bool structure_delta_observed = false;
    std::size_t terminal_public_materializations = 0u;
    CfaPhysicalWorkLedger terminal_presentation_work;
    PersistentWorkLedger terminal_function_value_presentation_work;
    CfaRoundLedger baseline_round;
    std::vector<CfaRoundLedger> delta_rounds;
};

struct CfaDeltaScaleGate final {
    CfaDeltaScaleSample n;
    CfaDeltaScaleSample eight_n;
};

[[nodiscard]] bool same_cfa_delta_round_work(
    const CfaRoundLedger& left,
    const CfaRoundLedger& right) {
    return left.iteration == right.iteration &&
           left.seeds == right.seeds &&
           left.cfa_bypass_reason == right.cfa_bypass_reason &&
           left.cfa_work == right.cfa_work &&
           left.function_value_work == right.function_value_work &&
           left.function_value_invocations ==
               right.function_value_invocations &&
           left.function_value_incremental.dirty_sccs ==
               right.function_value_incremental.dirty_sccs &&
           left.function_value_incremental.dirty_functions ==
               right.function_value_incremental.dirty_functions &&
           left.function_value_incremental.dirty_inventory_sinks ==
               right.function_value_incremental.dirty_inventory_sinks &&
           left.function_value_incremental.full_cpu_recompute_fallbacks ==
               right.function_value_incremental
                   .full_cpu_recompute_fallbacks;
}

[[nodiscard]] bool same_cfa_delta_rounds(
    const std::span<const CfaRoundLedger> left,
    const std::span<const CfaRoundLedger> right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      same_cfa_delta_round_work);
}

[[nodiscard]] CfaDeltaScaleSample run_cfa_delta_scale_sample(
    const std::size_t function_count) {
    auto image = make_cfa_delta_scale_image(function_count);
    CfaRoundCollector collector;
    const auto warm = katana::analysis::analyze_control_flow(
        image,
        nullptr,
        [&collector](const auto& progress) {
            collector.observe(progress);
        });
    auto collected = collector.finish();
    auto& rounds = collected.rounds;

    katana::analysis::AnalysisOverrides fresh_overrides;
    fresh_overrides.mode =
        katana::analysis::AnalysisDirectiveMode::Hint;
    fresh_overrides.functions.push_back({0x180u, 1u, 0u});
    const auto fresh = katana::analysis::analyze_control_flow(
        image, &fresh_overrides);

    require(katana::analysis::guarded_aot_inventory_complete(warm) &&
                katana::analysis::guarded_aot_inventory_complete(fresh) &&
                warm.persistent_analysis_bypass_reason ==
                    katana::analysis::PersistentAnalysisBypassReason::None &&
                fresh.persistent_analysis_bypass_reason ==
                    katana::analysis::PersistentAnalysisBypassReason::None &&
                warm.recursive_full_recompute_fallbacks == 0u &&
                fresh.recursive_full_recompute_fallbacks == 0u,
            "CFA-O(delta)-Fixture war abgeschnitten, umging Persistenz oder "
            "fiel auf einen Full-Recompute zurueck");
    if (warm.recursive.functions.size() != function_count ||
        fresh.recursive.functions.size() != function_count) {
        std::cerr << "[cfa delta-scale " << function_count
                  << " function-diff] warm="
                  << warm.recursive.functions.size() << " fresh="
                  << fresh.recursive.functions.size() << " expected="
                  << function_count << " warm_entries=";
        for (const auto& function : warm.recursive.functions)
            std::cerr << "0x" << std::hex << function.address << ',';
        std::cerr << " fresh_entries=";
        for (const auto& function : fresh.recursive.functions)
            std::cerr << "0x" << std::hex << function.address << ',';
        std::cerr << std::dec << '\n' << std::flush;
    }
    require(warm.recursive.functions.size() == function_count &&
                fresh.recursive.functions.size() == function_count,
            "CFA-O(delta)-Fixture verlor deklarierte Funktionen");
    if (rounds.size() < 3u) {
        std::cerr << "[cfa delta-scale " << function_count
                  << " round-diff] rounds=" << rounds.size();
        for (const auto& round : rounds) {
            std::cerr << " {iteration=" << round.iteration
                      << ",added=" << round.seeds.seed_facts_added
                      << ",changed=" << round.seeds.seed_targets_changed
                      << ",decode=" << round.seeds.decode_targets
                      << ",metadata=" << round.seeds.metadata_targets
                      << ",fva=" << round.function_value_invocations << '}';
        }
        std::cerr << " warm_seed_facts=";
        for (const auto& fact : warm.seed_facts)
            std::cerr << "0x" << std::hex << fact.target_address << ',';
        std::cerr << " fresh_seed_facts=";
        for (const auto& fact : fresh.seed_facts)
            std::cerr << "0x" << std::hex << fact.target_address << ',';
        std::cerr << " warm_guarded=";
        for (const auto& entry : warm.guarded_aot_entries)
            std::cerr << "0x" << std::hex << entry.guest_address << ',';
        std::cerr << " fresh_guarded=";
        for (const auto& entry : fresh.guarded_aot_entries)
            std::cerr << "0x" << std::hex << entry.guest_address << ',';
        std::cerr << std::dec << '\n' << std::flush;
    }
    require(rounds.size() >= 3u,
            "CFA-O(delta)-Fixture erzeugte keinen echten mehrstufigen "
            "Candidate-/Inventory-Fixpunkt");

    const auto candidate_resolution = std::find_if(
        warm.indirect_control_flow.begin(),
        warm.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x14Cu;
        });
    const auto guarded_callback = std::find_if(
        warm.guarded_aot_entries.begin(),
        warm.guarded_aot_entries.end(),
        [](const auto& entry) {
            return entry.guest_address == 0x1E0u;
        });
    const auto late_candidate_observed =
        candidate_resolution != warm.indirect_control_flow.end() &&
        candidate_resolution->targets.empty() &&
        candidate_resolution->analysis_candidates ==
            std::vector<std::uint32_t>{0x180u} &&
        guarded_callback != warm.guarded_aot_entries.end() &&
        has_cfa_seed_cause(
            warm,
            0x180u,
            katana::analysis::ControlFlowAnalysisResult::SeedCauseKind::
                IndirectAnalysisCandidate,
            0x14Cu) &&
        has_cfa_seed_cause(
            warm,
            0x1E0u,
            katana::analysis::ControlFlowAnalysisResult::SeedCauseKind::
                StoredCodeAddress,
            0x184u) &&
        has_cfa_seed_cause(
            fresh,
            0x180u,
            katana::analysis::ControlFlowAnalysisResult::SeedCauseKind::
                IndirectAnalysisCandidate,
            0x14Cu) &&
        has_cfa_seed_cause(
            fresh,
            0x1E0u,
            katana::analysis::ControlFlowAnalysisResult::SeedCauseKind::
                StoredCodeAddress,
            0x184u);

    std::vector<CfaRoundLedger> delta_rounds(
        rounds.begin() + 1, rounds.end());
    const auto structure_delta_observed = std::any_of(
        delta_rounds.begin(), delta_rounds.end(),
        [](const auto& round) {
            return round.seeds.decode_targets != 0u &&
                   round.cfa_work.recursive.decoded_work_items != 0u &&
                   round.cfa_work.recursive
                           .canonical_instruction_updates != 0u &&
                   round.function_value_work.program_graph_blocks_built !=
                       0u;
        });
    require(late_candidate_observed && structure_delta_observed,
            "Produktions-CFA-Gate besass keinen spaeten Candidate- plus "
            "Inventory-Seed in einen zuvor undekodierten Funktionsbereich");
    for (const auto& round : delta_rounds) {
        const auto round_is_delta_local =
            round.seeds.full_cpu_fallbacks == 0u &&
                round.cfa_bypass_reason ==
                    katana::analysis::PersistentAnalysisBypassReason::None &&
                round.cfa_work.avoided_global_prepasses(
                    round.seeds.seed_targets_changed) &&
                round.function_value_invocations != 0u &&
                round.function_value_work.bypass_reason ==
                    katana::analysis::PersistentAnalysisBypassReason::None &&
                round.function_value_work.avoided_full_edge_prepass() &&
                round.function_value_incremental.balanced() &&
                round.function_value_incremental
                        .analysis_epochs_discarded == 0u &&
                round.function_value_incremental
                        .full_cpu_recompute_fallbacks == 0u;
        if (!round_is_delta_local) {
            std::cerr << "[cfa delta-scale " << function_count
                      << " round " << round.iteration
                      << " locality-diff] cfa=";
            append_cfa_physical_work_json(std::cerr, round.cfa_work);
            std::cerr << " fva=";
            append_persistent_work_json(
                std::cerr, round.function_value_work);
            std::cerr << " seeds_fallbacks="
                      << round.seeds.full_cpu_fallbacks
                      << " cfa_bypass="
                      << katana::analysis::persistent_analysis_bypass_reason_name(
                             round.cfa_bypass_reason)
                      << " fva_invocations="
                      << round.function_value_invocations
                      << " incremental="
                      << round.function_value_incremental
                             .analysis_epochs_published
                      << '/'
                      << round.function_value_incremental
                             .analysis_epochs_discarded
                      << ",dirty="
                      << round.function_value_incremental.dirty_sccs
                      << '/'
                      << round.function_value_incremental.dirty_functions
                      << '/'
                      << round.function_value_incremental
                             .dirty_inventory_sinks
                      << ",fallbacks="
                      << round.function_value_incremental
                             .full_cpu_recompute_fallbacks
                      << '\n' << std::flush;
        }
        require(
            round_is_delta_local,
            "Spaete Produktions-CFA-Runde fuehrte globale "
            "Copy-/Shift-/Scan-/Sort-/Materialisierungsarbeit oder einen "
            "Fallback aus");
    }
    const auto semantic_comparison = same_cfa_semantics(warm, fresh);
    const auto warm_equals_fresh = semantic_comparison.equal;
    const auto terminal_public_materializations =
        warm.recursive_physical_work.public_materializations;
    const auto& presentation = collected.terminal_presentation_work;
    const auto& function_value_presentation =
        collected.terminal_function_value_presentation_work;
    const auto terminal_contract = warm_equals_fresh &&
                semantic_comparison
                    .synthetic_fresh_hint_normalization_verified &&
                warm.recursive_final_materializations == 1u &&
                fresh.recursive_final_materializations == 1u &&
                terminal_public_materializations == 1u &&
                fresh.recursive_physical_work.public_materializations == 1u &&
                presentation.recursive_final_materializations == 1u &&
                presentation.recursive.public_materializations == 1u &&
                presentation.recursive.public_baseline_hash_bytes != 0u &&
                presentation.recursive.public_baseline_copy_items != 0u &&
                presentation.recursive.public_sort_items != 0u &&
                presentation.recursive.public_materialized_items != 0u &&
                presentation.recursive.terminal_epoch_fold_items != 0u &&
                presentation.result_index_copy_items != 0u &&
                presentation.result_index_sort_items != 0u &&
                presentation.result_index_materialized_items != 0u &&
                presentation.recursive.trusted_snapshot_validations == 0u &&
                presentation.recursive.seed_arena_copy_items == 0u &&
                presentation.recursive.seed_arena_copy_bytes == 0u &&
                presentation.recursive.seed_arena_shift_items == 0u &&
                presentation.recursive.seed_arena_shift_bytes == 0u &&
                presentation.recursive.epoch_index_lookups == 0u &&
                presentation.recursive.epoch_index_updates == 0u &&
                presentation.recursive.seed_contract_items_visited == 0u &&
                presentation.recursive.decoded_work_items == 0u &&
                presentation.recursive.canonical_context_updates == 0u &&
                presentation.recursive.canonical_instruction_updates == 0u &&
                presentation.recursive.canonical_function_updates == 0u &&
                presentation.runtime_copy_instruction_visits == 0u &&
                presentation.runtime_copy_result_entries_visited == 0u &&
                presentation.runtime_copy_result_entries_rebuilt == 0u &&
                presentation.local_control_flow_instruction_visits == 0u &&
                presentation.local_control_flow_result_entries_visited ==
                    0u &&
                presentation.local_control_flow_result_entries_rebuilt ==
                    0u &&
                presentation.dispatch_index_entries_visited == 0u &&
                presentation.dispatch_index_entries_rebuilt == 0u &&
                presentation.jump_table_instruction_visits == 0u &&
                presentation.jump_table_result_entries_visited == 0u &&
                presentation.jump_table_result_entries_rebuilt == 0u &&
                presentation.function_boundary_entries_visited == 0u &&
                presentation.function_boundary_entries_rebuilt == 0u &&
                presentation.function_edge_family_entries_visited == 0u &&
                presentation.function_edge_family_entries_rebuilt == 0u &&
                presentation.function_edge_state_encode_items == 0u &&
                presentation.function_edge_state_copy_items == 0u &&
                presentation.function_edge_state_exact_compare_items == 0u;
    if (!terminal_contract) {
        std::cerr << "[cfa delta-scale " << function_count
                  << " terminal-diff] semantic=" << warm_equals_fresh
                  << " normalization="
                  << semantic_comparison
                         .synthetic_fresh_hint_normalization_verified
                  << " materializations="
                  << warm.recursive_final_materializations << '/'
                  << fresh.recursive_final_materializations << '/'
                  << terminal_public_materializations << '/'
                  << fresh.recursive_physical_work.public_materializations
                  << " presentation=";
        append_cfa_physical_work_json(std::cerr, presentation);
        std::cerr << '\n' << std::flush;
    }
    require(terminal_contract,
            "CFA-Warmzustand wich von der Fresh-Hint-Referenz ab oder "
            "materialisierte den oeffentlichen Recursive-Stand nicht exakt "
            "einmal terminal");
    require(
        function_value_presentation.bypass_reason ==
                katana::analysis::PersistentAnalysisBypassReason::None &&
            function_value_presentation.program_delta_entries_visited ==
                0u &&
            function_value_presentation.avoided_full_edge_prepass() &&
            function_value_presentation.program_graph_blocks_built == 0u &&
            function_value_presentation.program_graph_blocks_reused == 0u &&
            function_value_presentation.program_graph_sccs_built == 0u &&
            function_value_presentation.program_graph_sccs_reused == 0u &&
            function_value_presentation
                    .inventory_topology_entries_visited == 0u &&
            function_value_presentation
                    .resolution_preparation_entries_visited == 0u &&
            function_value_presentation
                    .resolution_dependency_nodes_built == 0u &&
            function_value_presentation
                    .resolution_dependency_nodes_reused == 0u &&
            function_value_presentation
                    .resolution_dependency_sccs_built == 0u &&
            function_value_presentation
                    .resolution_dependency_sccs_reused == 0u &&
            function_value_presentation.abi_contract_entries_visited ==
                0u &&
            function_value_presentation.abi_contract_entries_rebuilt ==
                0u &&
            function_value_presentation
                    .summary_candidate_entries_visited == 0u &&
            function_value_presentation
                    .summary_candidate_entries_rebuilt == 0u &&
            function_value_presentation.final_materialized_blocks != 0u &&
            function_value_presentation.final_materialized_functions != 0u,
        "CFA-Terminalpfad fuehrte neben der FVA-Praesentationsmaterialisierung "
        "unerlaubte Analysearbeit aus");

    return {function_count,
            warm.recursive.instructions.size(),
            warm_equals_fresh,
            semantic_comparison
                .synthetic_fresh_hint_normalization_verified,
            late_candidate_observed,
            structure_delta_observed,
            terminal_public_materializations,
            std::move(collected.terminal_presentation_work),
            std::move(
                collected.terminal_function_value_presentation_work),
            std::move(rounds.front()),
            std::move(delta_rounds)};
}

[[nodiscard]] CfaDeltaScaleGate run_cfa_delta_scale_gate() {
    constexpr std::size_t n = 16u;
    constexpr std::size_t eight_n = n * 8u;
    CfaDeltaScaleGate gate{
        run_cfa_delta_scale_sample(n),
        run_cfa_delta_scale_sample(eight_n)};
    const auto scale_contract =
        gate.eight_n.function_count == gate.n.function_count * 8u &&
            gate.eight_n.instruction_count > gate.n.instruction_count &&
            gate.eight_n.baseline_round.cfa_work.recursive
                    .decoded_work_items >
                gate.n.baseline_round.cfa_work.recursive
                    .decoded_work_items &&
            gate.eight_n.baseline_round.cfa_work
                    .local_control_flow_instruction_visits >
                gate.n.baseline_round.cfa_work
                    .local_control_flow_instruction_visits &&
            same_cfa_delta_rounds(
                gate.eight_n.delta_rounds,
                gate.n.delta_rounds);
    if (!scale_contract) {
        std::cerr << "[cfa delta-scale gate-diff] functions="
                  << gate.n.function_count << '/'
                  << gate.eight_n.function_count << " instructions="
                  << gate.n.instruction_count << '/'
                  << gate.eight_n.instruction_count << " baseline-decodes="
                  << gate.n.baseline_round.cfa_work.recursive
                         .decoded_work_items
                  << '/'
                  << gate.eight_n.baseline_round.cfa_work.recursive
                         .decoded_work_items
                  << " baseline-local="
                  << gate.n.baseline_round.cfa_work
                         .local_control_flow_instruction_visits
                  << '/'
                  << gate.eight_n.baseline_round.cfa_work
                         .local_control_flow_instruction_visits
                  << " delta-rounds=" << gate.n.delta_rounds.size() << '/'
                  << gate.eight_n.delta_rounds.size() << '\n';
        const auto count = std::min(gate.n.delta_rounds.size(),
                                    gate.eight_n.delta_rounds.size());
        for (std::size_t index = 0u; index < count; ++index) {
            const auto& n_round = gate.n.delta_rounds[index];
            const auto& eight_n_round = gate.eight_n.delta_rounds[index];
            if (same_cfa_delta_round_work(n_round, eight_n_round)) continue;
            std::cerr << "[cfa delta-scale round " << index
                      << " diff] n-cfa=";
            append_cfa_physical_work_json(std::cerr, n_round.cfa_work);
            std::cerr << " eight-n-cfa=";
            append_cfa_physical_work_json(
                std::cerr, eight_n_round.cfa_work);
            std::cerr << " n-fva=";
            append_persistent_work_json(
                std::cerr, n_round.function_value_work);
            std::cerr << " eight-n-fva=";
            append_persistent_work_json(
                std::cerr, eight_n_round.function_value_work);
            std::cerr << " seeds=" << n_round.seeds.seed_facts_added << '/'
                      << n_round.seeds.seed_targets_changed << '/'
                      << n_round.seeds.decode_targets << '/'
                      << n_round.seeds.metadata_targets << " vs "
                      << eight_n_round.seeds.seed_facts_added << '/'
                      << eight_n_round.seeds.seed_targets_changed << '/'
                      << eight_n_round.seeds.decode_targets << '/'
                      << eight_n_round.seeds.metadata_targets
                      << " dirty="
                      << n_round.function_value_incremental.dirty_sccs
                      << '/'
                      << n_round.function_value_incremental.dirty_functions
                      << '/'
                      << n_round.function_value_incremental
                             .dirty_inventory_sinks
                      << " vs "
                      << eight_n_round.function_value_incremental.dirty_sccs
                      << '/'
                      << eight_n_round.function_value_incremental
                             .dirty_functions
                      << '/'
                      << eight_n_round.function_value_incremental
                             .dirty_inventory_sinks
                      << '\n';
        }
        std::cerr << std::flush;
    }
    require(
        scale_contract,
        "Produktions-CFA-N-vs-8N-Gate skaliert nach der Baseline mit dem "
        "Gesamtprogramm oder besitzt keine echte groessere Referenz");
    return gate;
}

void append_incremental_epoch_json(
    std::ostream& output,
    const IncrementalEpochLedger& ledger) {
    output << "{\"analysis_epochs_published\":"
           << ledger.analysis_epochs_published
           << ",\"analysis_epochs_discarded\":"
           << ledger.analysis_epochs_discarded
           << ",\"incremental_epochs_started\":"
           << ledger.incremental_epochs_started
           << ",\"resolution_root_artifacts_total\":"
           << ledger.resolution_root_artifacts_total
           << ",\"resolution_root_artifacts_reused\":"
           << ledger.resolution_root_artifacts_reused
           << ",\"resolution_root_artifacts_recomputed\":"
           << ledger.resolution_root_artifacts_recomputed
           << ",\"resolution_root_artifacts_retained\":"
           << ledger.resolution_root_artifacts_retained
           << ",\"resolution_epoch_retained_bytes\":"
           << ledger.resolution_epoch_retained_bytes
           << ",\"resolution_retention_limit_reason\":\""
           << katana::analysis::resolution_retention_limit_reason_name(
                  ledger.resolution_retention_limit_reason)
           << "\",\"dirty_sccs\":" << ledger.dirty_sccs
           << ",\"dirty_functions\":" << ledger.dirty_functions
           << ",\"dirty_inventory_sinks\":"
           << ledger.dirty_inventory_sinks
           << ",\"full_cpu_recompute_fallbacks\":"
           << ledger.full_cpu_recompute_fallbacks << '}';
}

void append_cfa_round_json(
    std::ostream& output,
    const CfaRoundLedger& round) {
    output << "{\"iteration\":" << round.iteration
           << ",\"seed_facts_added\":"
           << round.seeds.seed_facts_added
           << ",\"seed_targets_changed\":"
           << round.seeds.seed_targets_changed
           << ",\"decode_targets\":"
           << round.seeds.decode_targets
           << ",\"metadata_targets\":"
           << round.seeds.metadata_targets
           << ",\"full_cpu_fallbacks\":"
           << round.seeds.full_cpu_fallbacks
           << ",\"persistent_analysis_bypass_reason\":\""
           << katana::analysis::persistent_analysis_bypass_reason_name(
                  round.cfa_bypass_reason)
           << "\""
           << ",\"function_value_invocations\":"
           << round.function_value_invocations
           << ",\"cfa_work\":";
    append_cfa_physical_work_json(output, round.cfa_work);
    output << ",\"function_value_work\":";
    append_persistent_work_json(output, round.function_value_work);
    output << ",\"function_value_incremental\":";
    append_incremental_epoch_json(
        output, round.function_value_incremental);
    output << '}';
}

void append_cfa_delta_scale_sample_json(
    std::ostream& output,
    const CfaDeltaScaleSample& sample) {
    output << "{\"function_count\":" << sample.function_count
           << ",\"instruction_count\":" << sample.instruction_count
           << ",\"warm_equals_fresh\":"
           << (sample.warm_equals_fresh ? "true" : "false")
           << ",\"synthetic_fresh_hint_normalization_verified\":"
           << (sample.synthetic_fresh_hint_normalization_verified
                   ? "true"
                   : "false")
           << ",\"late_candidate_observed\":"
           << (sample.late_candidate_observed ? "true" : "false")
           << ",\"structure_delta_observed\":"
           << (sample.structure_delta_observed ? "true" : "false")
           << ",\"terminal_public_materializations\":"
           << sample.terminal_public_materializations
           << ",\"terminal_presentation_work\":";
    append_cfa_physical_work_json(
        output, sample.terminal_presentation_work);
    output << ",\"terminal_function_value_presentation_work\":";
    append_persistent_work_json(
        output, sample.terminal_function_value_presentation_work);
    output
           << ",\"baseline_round\":";
    append_cfa_round_json(output, sample.baseline_round);
    output << ",\"delta_rounds\":[";
    for (std::size_t index = 0u;
         index < sample.delta_rounds.size(); ++index) {
        if (index != 0u) output << ',';
        append_cfa_round_json(output, sample.delta_rounds[index]);
    }
    output << "]}";
}

struct ExactAotCounts final {
    std::size_t modules = 0u;
    std::size_t entries = 0u;
    std::size_t source_bindings = 0u;
};

[[nodiscard]] ExactAotCounts verify_exact_discovery(
    const katana::platform::DreamcastDiscBoot& disc,
    const std::span<const katana::codegen::LatentAotEntryHint> hints,
    const std::size_t workers,
    const katana::ProgressReporter& reporter) {
    katana::codegen::LatentAotDiscoveryOptions options;
    options.mode = katana::codegen::LatentAotDiscoveryMode::ExactOnly;
    options.maximum_workers = workers;
    options.progress = reporter;
    const auto discovery = katana::codegen::discover_latent_aot_modules(
        disc.source,
        disc.data_track_lba,
        disc.extent_lba_bias,
        std::span<const std::string>{},
        options,
        std::span<const katana::codegen::LatentAotOccupiedRange>{},
        hints);

    std::set<std::pair<std::string, std::uint32_t>> expected_modules;
    for (const auto& hint : hints)
        expected_modules.emplace(hint.byte_identity, hint.byte_size);
    require(discovery.modules.size() == expected_modules.size() &&
                discovery.analysis_full_pipeline_runs == discovery.modules.size(),
            "ExactOnly analysierte nicht jedes deklarierte Modul real");

    ExactAotCounts counts;
    counts.modules = discovery.modules.size();
    bool saw_multi_extent_multi_entry_template = false;
    for (const auto& module : discovery.modules) {
        std::vector<std::uint32_t> expected_entries;
        std::set<std::pair<std::uint64_t, std::uint32_t>> expected_bindings;
        for (const auto& hint : hints) {
            if (hint.byte_identity != module.byte_identity ||
                hint.byte_size != module.byte_size)
                continue;
            expected_entries.push_back(hint.module_relative_offset);
            expected_bindings.emplace(hint.disc_byte_offset, hint.byte_size);
        }
        std::sort(expected_entries.begin(), expected_entries.end());
        expected_entries.erase(
            std::unique(expected_entries.begin(), expected_entries.end()),
            expected_entries.end());
        require(!expected_entries.empty() &&
                    module.entry_offsets == expected_entries &&
                    module.source_bindings.size() == expected_bindings.size(),
                "ExactOnly verlor deklarierte Entries oder Source-Bindings");
        for (const auto& binding : module.source_bindings)
            require(expected_bindings.contains(
                        {binding.disc_byte_offset, binding.byte_size}),
                    "ExactOnly erzeugte eine nicht deklarierte Source-Bindung");
        for (const auto offset : module.entry_offsets) {
            const auto entry = module.source_address + offset;
            require(std::any_of(module.program.begin(), module.program.end(),
                                [&](const auto& function) {
                                    return std::any_of(
                                        function.blocks.begin(),
                                        function.blocks.end(),
                                        [&](const auto& block) {
                                            return block.start_address == entry;
                                        });
                                }),
                    "ExactOnly-Entry besitzt keinen realen nativen IR-Block");
        }
        counts.entries += module.entry_offsets.size();
        counts.source_bindings += module.source_bindings.size();
        saw_multi_extent_multi_entry_template |=
            module.entry_offsets.size() == 2u &&
            module.source_bindings.size() == 2u;
    }
    require(saw_multi_extent_multi_entry_template,
            "ExactOnly deduplizierte das bytegleiche Multi-Extent-Template nicht");
    return counts;
}

[[nodiscard]] std::vector<std::filesystem::path> emitted_translation_units(
    const std::filesystem::path& output_root) {
    const auto code_root = output_root / "generated" / "code";
    std::error_code error;
    require(std::filesystem::is_directory(code_root, error) && !error,
            "Export besitzt kein generiertes Codeverzeichnis");
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(code_root)) {
        const auto status = entry.symlink_status(error);
        require(!error && !std::filesystem::is_symlink(status),
                "generierter Code darf kein Symlink sein");
        if (!std::filesystem::is_regular_file(status)) continue;
        const auto name = entry.path().filename().string();
        if (name.starts_with("unit-v") && entry.path().extension() == ".cpp")
            result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

void require_cold_output(const std::filesystem::path& output_root) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(output_root, error);
    if (error == std::errc::no_such_file_or_directory) return;
    require(!error && std::filesystem::is_directory(status) &&
                !std::filesystem::is_symlink(status),
            "Portausgabe ist kein sicheres kaltes Verzeichnis");
    require(std::filesystem::directory_iterator(output_root) ==
                std::filesystem::directory_iterator(),
            "Portausgabe muss fuer den Kaltstress leer sein");
}

[[nodiscard]] std::string read_port_metadata(
    const std::filesystem::path& output_root) {
    return read_regular_text(
        output_root / "generated" / "metadata" / "port-project.json");
}

struct StructuredProgressEvidence final {
    std::size_t events = 0u;
    std::size_t maximum_inter_record_gap_milliseconds = 0u;
    std::size_t source_configured_workers = 0u;
    std::size_t latent_configured_workers = 0u;
    bool function_value_cache_ledger = false;
    bool function_value_physical_work = false;
    bool cfa_physical_work_ledger = false;
    bool function_value_persistent_work_ledger = false;
    bool growing_workset_observed = false;
    bool head_of_line_observed = false;
    bool seed_round_ledger = false;
    bool incremental_epoch_ledger = false;
    bool all_required_operations_terminal = false;
    bool all_function_subphases_terminal = false;
};

[[nodiscard]] StructuredProgressEvidence verify_structured_progress(
    std::vector<katana::ProgressEvent> events,
    const std::size_t expected_source_workers,
    const std::size_t expected_partitions,
    const std::size_t expected_latent_workers,
    const std::size_t expected_latent_modules) {
    require(!events.empty(), "realer Structured-Progress blieb leer");
    std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        return left.sequence < right.sequence;
    });
    StructuredProgressEvidence evidence;
    evidence.events = events.size();
    std::uint64_t previous_sequence = 0u;
    std::uint64_t previous_elapsed = 0u;
    for (const auto& event : events) {
        require(event.sequence > previous_sequence &&
                    event.elapsed_milliseconds >= previous_elapsed &&
                    katana::progress_event_telemetry_complete(event),
                "Structured-Progress ist ungeordnet oder telemetry-incomplete");
        evidence.maximum_inter_record_gap_milliseconds = std::max(
            evidence.maximum_inter_record_gap_milliseconds,
            static_cast<std::size_t>(
                event.elapsed_milliseconds - previous_elapsed));
        previous_sequence = event.sequence;
        previous_elapsed = event.elapsed_milliseconds;
        if (event.counters.growing_workset == true &&
            event.counters.added_work && *event.counters.added_work > 0u)
            evidence.growing_workset_observed = true;
        if (event.counters.head_of_line_index &&
            event.counters.head_of_line_elapsed_milliseconds &&
            event.counters.ready_ahead && *event.counters.ready_ahead > 0u)
            evidence.head_of_line_observed = true;
        if (event.operation ==
                katana::ProgressOperation::ControlFlowAnalysis &&
            event.state == katana::ProgressState::Completed &&
            event.counters.round_seed_facts_added &&
            event.counters.round_seed_targets_changed &&
            event.counters.round_decode_targets &&
            event.counters.round_metadata_targets &&
            event.counters.round_full_cpu_fallbacks)
            evidence.seed_round_ledger = true;
        if (event.operation ==
                katana::ProgressOperation::ControlFlowAnalysis &&
            event.state == katana::ProgressState::Completed) {
            const auto& counters = event.counters;
            const std::array<const std::optional<std::uint64_t>*, 41u>
                cfa_physical_counters{
                    &counters.recursive_snapshot_epochs,
                    &counters.recursive_final_materializations,
                    &counters.recursive_trusted_snapshot_validations,
                    &counters.recursive_seed_arena_copy_items,
                    &counters.recursive_seed_arena_copy_bytes,
                    &counters.recursive_seed_arena_shift_items,
                    &counters.recursive_seed_arena_shift_bytes,
                    &counters.epoch_index_lookups,
                    &counters.epoch_index_updates,
                    &counters.terminal_epoch_fold_items,
                    &counters.recursive_seed_contract_items_visited,
                    &counters.recursive_decoded_work_items,
                    &counters.recursive_canonical_context_updates,
                    &counters.recursive_canonical_instruction_updates,
                    &counters.recursive_canonical_function_updates,
                    &counters.recursive_public_baseline_hash_bytes,
                    &counters.recursive_public_baseline_copy_items,
                    &counters.recursive_public_sort_items,
                    &counters.recursive_public_materialized_items,
                    &counters.recursive_public_materializations,
                    &counters.runtime_copy_instruction_visits,
                    &counters.runtime_copy_result_entries_visited,
                    &counters.runtime_copy_result_entries_rebuilt,
                    &counters.local_control_flow_instruction_visits,
                    &counters.local_control_flow_result_entries_visited,
                    &counters.local_control_flow_result_entries_rebuilt,
                    &counters.dispatch_index_entries_visited,
                    &counters.dispatch_index_entries_rebuilt,
                    &counters.jump_table_instruction_visits,
                    &counters.jump_table_result_entries_visited,
                    &counters.jump_table_result_entries_rebuilt,
                    &counters.function_boundary_entries_visited,
                    &counters.function_boundary_entries_rebuilt,
                    &counters.function_edge_family_entries_visited,
                    &counters.function_edge_family_entries_rebuilt,
                    &counters.function_edge_state_encode_items,
                    &counters.function_edge_state_copy_items,
                    &counters.function_edge_state_exact_compare_items,
                    &counters.result_index_copy_items,
                    &counters.result_index_sort_items,
                    &counters.result_index_materialized_items};
            evidence.cfa_physical_work_ledger =
                counters.persistent_analysis_bypass_reason.has_value() &&
                std::all_of(
                    cfa_physical_counters.begin(),
                    cfa_physical_counters.end(),
                    [](const auto* value) {
                        return value->has_value();
                    });
        }
        if (event.operation == katana::ProgressOperation::FunctionValueAnalysis) {
            const auto& counters = event.counters;
            if (event.state == katana::ProgressState::Completed) {
                const std::array<
                    const std::optional<std::uint64_t>*, 23u>
                    persistent_work_counters{
                        &counters.program_delta_entries_visited,
                        &counters.function_edge_full_scans,
                        &counters.function_edge_full_sorts,
                        &counters.candidate_call_edge_full_scans,
                        &counters.candidate_call_edge_full_sorts,
                        &counters.candidate_tail_edge_full_scans,
                        &counters.candidate_tail_edge_full_sorts,
                        &counters.program_graph_blocks_built,
                        &counters.program_graph_blocks_reused,
                        &counters.program_graph_sccs_built,
                        &counters.program_graph_sccs_reused,
                        &counters.inventory_topology_entries_visited,
                        &counters.resolution_preparation_entries_visited,
                        &counters.resolution_dependency_nodes_built,
                        &counters.resolution_dependency_nodes_reused,
                        &counters.resolution_dependency_sccs_built,
                        &counters.resolution_dependency_sccs_reused,
                        &counters.abi_contract_entries_visited,
                        &counters.abi_contract_entries_rebuilt,
                        &counters.summary_candidate_entries_visited,
                        &counters.summary_candidate_entries_rebuilt,
                        &counters.final_materialized_blocks,
                        &counters.final_materialized_functions};
                evidence.function_value_persistent_work_ledger =
                    evidence.function_value_persistent_work_ledger ||
                    (counters.persistent_analysis_bypass_reason.has_value() &&
                     std::all_of(
                         persistent_work_counters.begin(),
                         persistent_work_counters.end(),
                         [](const auto* value) {
                             return value->has_value();
                         }));
            }
            if (event.state == katana::ProgressState::Completed &&
                counters.analysis_epochs_published &&
                counters.analysis_epochs_discarded &&
                counters.incremental_epochs_started &&
                counters.resolution_root_artifacts_total &&
                counters.resolution_root_artifacts_reused &&
                counters.resolution_root_artifacts_recomputed &&
                counters.resolution_root_artifacts_retained &&
                counters.resolution_epoch_retained_bytes &&
                counters.resolution_retention_limit_reason &&
                counters.dirty_sccs && counters.dirty_functions &&
                counters.dirty_inventory_sinks &&
                counters.full_cpu_recompute_fallbacks) {
                require(
                    *counters.incremental_epochs_started ==
                            *counters.analysis_epochs_published +
                                *counters.analysis_epochs_discarded &&
                        *counters.resolution_root_artifacts_total ==
                            *counters.resolution_root_artifacts_reused +
                                *counters
                                     .resolution_root_artifacts_recomputed &&
                        *counters.resolution_root_artifacts_retained <=
                            *counters.resolution_root_artifacts_total &&
                        (*counters.resolution_root_artifacts_retained == 0u ||
                         *counters.resolution_epoch_retained_bytes != 0u) &&
                        *counters.resolution_retention_limit_reason == "none",
                    "terminale inkrementelle FVA-Epochenbilanz driftet");
                evidence.incremental_epoch_ledger = true;
            }
            const std::array<const std::optional<std::uint64_t>*, 12u>
                primary_miss_counts{
                    &counters.cache_miss_cold,
                    &counters.cache_miss_evicted,
                    &counters.cache_miss_oversize_or_no_exact_replay,
                    &counters.cache_miss_function_shape_changed,
                    &counters.cache_miss_projected_ingress_changed,
                    &counters.cache_miss_summary_dependency_changed,
                    &counters.cache_miss_abi_contract_changed,
                    &counters.cache_miss_resolution_lens_changed,
                    &counters.cache_miss_inventory_sink_changed,
                    &counters.cache_miss_isolation_partition_changed,
                    &counters.cache_miss_contextual_summary_changed,
                    &counters.cache_miss_tail_ingress_changed};
            const auto complete_miss_reasons = std::all_of(
                primary_miss_counts.begin(),
                primary_miss_counts.end(),
                [](const auto* value) { return value->has_value(); });
            std::uint64_t classified_misses = 0u;
            if (complete_miss_reasons)
                for (const auto* value : primary_miss_counts)
                    classified_misses += **value;
            const auto ledger_balanced =
                counters.cache_lookups && counters.cache_ready_hits &&
                counters.cache_in_flight_coalesces && counters.cache_hits &&
                counters.cache_misses && counters.physical_evaluations &&
                counters.cache_replay_fallback_recomputes &&
                counters.cache_diagnostic_bypass_evaluations &&
                counters.multi_root_context_requests &&
                counters.multi_root_unique_contexts &&
                counters.multi_root_ready_reuses &&
                counters.multi_root_in_flight_reuses &&
                counters.multi_root_provenance_links &&
                counters.multi_root_retained_contexts &&
                counters.multi_root_retained_payload_bytes &&
                counters.multi_root_evictions &&
                counters.executor_running_workers &&
                counters.executor_waiting_workers &&
                counters.executor_idle_workers &&
                counters.executor_queued_work &&
                counters.executor_memory_blocked_work &&
                counters.executor_continuations &&
                counters.analysis_memory_capacity_bytes &&
                counters.analysis_memory_used_bytes &&
                counters.analysis_memory_peak_bytes &&
                counters.cache_evictions && counters.cache_entries &&
                counters.cache_retained_payload_bytes && complete_miss_reasons &&
                *counters.cache_lookups ==
                    *counters.cache_ready_hits +
                        *counters.cache_in_flight_coalesces +
                        *counters.cache_misses &&
                *counters.cache_hits ==
                    *counters.cache_ready_hits +
                        *counters.cache_in_flight_coalesces &&
                *counters.cache_misses == classified_misses &&
                *counters.physical_evaluations ==
                    *counters.cache_misses +
                        *counters.cache_replay_fallback_recomputes +
                        *counters.cache_diagnostic_bypass_evaluations &&
                *counters.multi_root_context_requests ==
                    *counters.multi_root_unique_contexts +
                        *counters.multi_root_ready_reuses +
                        *counters.multi_root_in_flight_reuses &&
                *counters.multi_root_retained_contexts <=
                    *counters.multi_root_unique_contexts &&
                *counters.multi_root_evictions ==
                    *counters.multi_root_unique_contexts -
                        *counters.multi_root_retained_contexts &&
                counters.active_workers &&
                *counters.active_workers ==
                    *counters.executor_running_workers &&
                *counters.executor_idle_workers <=
                    *counters.executor_waiting_workers &&
                *counters.executor_memory_blocked_work <=
                    *counters.executor_queued_work &&
                *counters.executor_continuations <=
                    *counters.executor_queued_work &&
                *counters.analysis_memory_used_bytes <=
                    *counters.analysis_memory_peak_bytes &&
                *counters.analysis_memory_peak_bytes <=
                    *counters.analysis_memory_capacity_bytes &&
                ((*counters.multi_root_retained_contexts == 0u) ==
                 (*counters.multi_root_retained_payload_bytes == 0u));
            const auto terminal_quiescent =
                counters.active_workers &&
                *counters.active_workers == 0u &&
                counters.active_evaluation_requests &&
                *counters.active_evaluation_requests == 0u &&
                counters.active_cache_key_builds &&
                *counters.active_cache_key_builds == 0u &&
                counters.active_cache_waits &&
                *counters.active_cache_waits == 0u &&
                counters.active_cache_replays &&
                *counters.active_cache_replays == 0u &&
                counters.active_physical_evaluations &&
                *counters.active_physical_evaluations == 0u &&
                counters.active_cache_commits &&
                *counters.active_cache_commits == 0u &&
                counters.planned_work && *counters.planned_work > 0u &&
                counters.started && counters.ready_work &&
                counters.committed_work && counters.queued_work &&
                *counters.started == *counters.planned_work &&
                *counters.ready_work == 0u &&
                *counters.committed_work == *counters.planned_work &&
                *counters.queued_work == 0u;
            const auto terminal_logical_balanced =
                ledger_balanced && counters.evaluation_requests &&
                *counters.evaluation_requests ==
                    *counters.cache_lookups +
                        *counters.multi_root_ready_reuses +
                        *counters.multi_root_in_flight_reuses +
                        *counters.cache_diagnostic_bypass_evaluations;
            if (terminal_logical_balanced &&
                event.state == katana::ProgressState::Completed &&
                *counters.cache_lookups > 0u && terminal_quiescent)
                evidence.function_value_cache_ledger = true;
            if (ledger_balanced && *counters.cache_misses > 0u &&
                *counters.physical_evaluations > 0u)
                evidence.function_value_physical_work = true;
        }
        if (event.operation == katana::ProgressOperation::SourceGeneration &&
            event.label == "partition-source-generation" &&
            event.state == katana::ProgressState::Completed) {
            require(event.total == expected_partitions &&
                        event.completed == expected_partitions &&
                        event.counters.configured_workers ==
                            expected_source_workers &&
                        event.counters.started == expected_partitions &&
                        event.counters.committed_work == expected_partitions &&
                        event.counters.active_workers == 0u &&
                        event.counters.queued_work == 0u,
                    "SourceGeneration meldet keinen exakten Worker-/Workledger");
            evidence.source_configured_workers = expected_source_workers;
        }
        if (event.operation == katana::ProgressOperation::CandidateResolution &&
            event.label == "latent-aot-candidates" &&
            event.state == katana::ProgressState::Completed) {
            require(event.total == expected_latent_modules &&
                        event.completed == expected_latent_modules &&
                        event.counters.configured_workers ==
                            expected_latent_workers &&
                        event.counters.started == expected_latent_modules &&
                        event.counters.committed_work == expected_latent_modules &&
                        event.counters.active_workers == 0u &&
                        event.counters.executor_running_workers ==
                            event.counters.active_workers &&
                        event.counters.executor_waiting_workers.has_value() &&
                        event.counters.executor_idle_workers.has_value() &&
                        event.counters.executor_queued_work.has_value() &&
                        event.counters.executor_memory_blocked_work
                            .has_value() &&
                        event.counters.executor_continuations.has_value() &&
                        event.counters.analysis_memory_capacity_bytes
                            .has_value() &&
                        event.counters.analysis_memory_used_bytes.has_value() &&
                        event.counters.analysis_memory_peak_bytes.has_value() &&
                        event.counters.queued_work == 0u,
                    "Latent-AOT meldet keinen exakten Worker-/Workledger");
            evidence.latent_configured_workers = expected_latent_workers;
        }
    }
    const std::array required_operations{
        katana::ProgressOperation::ProgramValidation,
        katana::ProgressOperation::ControlFlowAnalysis,
        katana::ProgressOperation::CandidateContractIteration,
        katana::ProgressOperation::FunctionValueAnalysis,
        katana::ProgressOperation::CandidateResolution,
        katana::ProgressOperation::LatentAotAnalysis,
        katana::ProgressOperation::IrGeneration,
        katana::ProgressOperation::IrOptimization,
        katana::ProgressOperation::SourceGeneration,
        katana::ProgressOperation::MetadataGeneration,
        katana::ProgressOperation::ArtifactWrite,
    };
    std::vector<katana::ProgressOperation> missing_required_operations;
    evidence.all_required_operations_terminal = std::all_of(
        required_operations.begin(), required_operations.end(),
        [&](const auto operation) {
            const auto terminal = std::any_of(
                events.begin(), events.end(), [&](const auto& event) {
                return event.operation == operation &&
                       event.state == katana::ProgressState::Completed;
            });
            if (!terminal) missing_required_operations.push_back(operation);
            return terminal;
        });
    const std::array required_subphases{
        std::string_view{"inventory-region-closure"},
        std::string_view{"inventory-region-sink-sources"},
        std::string_view{"abi-return-signatures"},
        std::string_view{"abi-stack-reads"},
        std::string_view{"abi-register-reads"},
        std::string_view{"persistent-store-signatures"},
        std::string_view{"inventory-reachability"},
        std::string_view{"cache-key-plan"},
        std::string_view{"resolution-root-dependencies"},
        std::string_view{"resolution-root-scc-order"},
        std::string_view{"resolution-root-scc-components"},
        std::string_view{"resolution-root-contracts"},
        std::string_view{"resolution-root-plan"},
    };
    const auto completed_scope =
        [&](const std::uint64_t scope_id,
            const katana::ProgressOperation operation,
            const std::string_view label = {})
            -> const katana::ProgressEvent* {
        const auto found = std::find_if(
            events.begin(), events.end(),
            [&](const auto& event) {
                return event.scope_id == scope_id &&
                       event.operation == operation &&
                       event.state == katana::ProgressState::Completed &&
                       (label.empty() || event.label == label);
            });
        return found == events.end() ? nullptr : &*found;
    };
    const auto completed_candidate_fva_parent =
        [&](const katana::ProgressEvent& subphase) {
        if (subphase.operation !=
                katana::ProgressOperation::FunctionValueAnalysis ||
            !subphase.parent_scope_id)
            return false;
        const auto* function_values = completed_scope(
            *subphase.parent_scope_id,
            katana::ProgressOperation::FunctionValueAnalysis,
            "function-value-analysis");
        if (function_values == nullptr ||
            !function_values->parent_scope_id)
            return false;
        const auto* candidate = completed_scope(
            *function_values->parent_scope_id,
            katana::ProgressOperation::CandidateContractIteration);
        if (candidate == nullptr || !candidate->parent_scope_id)
            return false;
        const auto* round = completed_scope(
            *candidate->parent_scope_id,
            katana::ProgressOperation::ControlFlowRound);
        if (round == nullptr || !round->parent_scope_id)
            return false;
        return completed_scope(
                   *round->parent_scope_id,
                   katana::ProgressOperation::ControlFlowAnalysis,
                   "kr4974-real-cfa") != nullptr;
    };
    evidence.all_function_subphases_terminal = std::all_of(
        required_subphases.begin(), required_subphases.end(),
        [&](const auto label) {
            return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.label == label &&
                       event.state == katana::ProgressState::Completed &&
                       completed_candidate_fva_parent(event);
            });
        });
    const auto complete =
        evidence.source_configured_workers == expected_source_workers &&
        evidence.latent_configured_workers == expected_latent_workers &&
        evidence.function_value_cache_ledger &&
        evidence.function_value_physical_work &&
        evidence.cfa_physical_work_ledger &&
        evidence.function_value_persistent_work_ledger &&
        evidence.growing_workset_observed &&
        evidence.head_of_line_observed &&
        evidence.seed_round_ledger &&
        evidence.incremental_epoch_ledger &&
        evidence.all_required_operations_terminal &&
        evidence.all_function_subphases_terminal &&
        evidence.maximum_inter_record_gap_milliseconds <= 10'000u;
    if (!complete) {
        std::ostringstream message;
        message << "Structured-E2E-Progress verlor Operationen, Subphasen "
                   "oder Heartbeats: source_workers="
                << evidence.source_configured_workers << '/'
                << expected_source_workers << " latent_workers="
                << evidence.latent_configured_workers << '/'
                << expected_latent_workers << " cache_ledger="
                << evidence.function_value_cache_ledger
                << " physical_work="
                << evidence.function_value_physical_work
                << " cfa_physical_ledger="
                << evidence.cfa_physical_work_ledger
                << " fva_persistent_ledger="
                << evidence.function_value_persistent_work_ledger
                << " growing=" << evidence.growing_workset_observed
                << " hol=" << evidence.head_of_line_observed
                << " seed_ledger=" << evidence.seed_round_ledger
                << " incremental_ledger="
                << evidence.incremental_epoch_ledger
                << " operations="
                << evidence.all_required_operations_terminal
                << " missing_operations=";
        if (missing_required_operations.empty()) {
            message << "none";
        } else {
            for (std::size_t index = 0u;
                 index < missing_required_operations.size(); ++index) {
                if (index != 0u) message << ',';
                const auto operation = missing_required_operations[index];
                message << katana::progress_operation_name(operation) << '[';
                constexpr std::array states{
                    katana::ProgressState::Started,
                    katana::ProgressState::Running,
                    katana::ProgressState::Heartbeat,
                    katana::ProgressState::Completed,
                    katana::ProgressState::Cached,
                    katana::ProgressState::Skipped,
                    katana::ProgressState::Failed};
                bool first_state = true;
                for (const auto state : states) {
                    const auto count = std::count_if(
                        events.begin(), events.end(),
                        [&](const auto& event) {
                            return event.operation == operation &&
                                   event.state == state;
                        });
                    if (count == 0u) continue;
                    if (!first_state) message << ',';
                    first_state = false;
                    message << katana::progress_state_name(state)
                            << ':' << count;
                }
                if (first_state) message << "absent";
                message << ']';
            }
        }
        message
                << " subphases="
                << evidence.all_function_subphases_terminal
                << " max_gap_ms="
                << evidence.maximum_inter_record_gap_milliseconds;
        fail(message.str());
    }
    return evidence;
}

} // namespace

int main(const int argc, char** argv) {
    const auto started = std::chrono::steady_clock::now();
    try {
        if (argc != 4) {
            std::cerr << "usage: katana-native-disc-cold-build-stress "
                         "<fixture-root> <port-output> <maximum-workers>\n";
            return 2;
        }
        const auto fixture_root =
            std::filesystem::absolute(argv[1]).lexically_normal();
        const auto output_root =
            std::filesystem::absolute(argv[2]).lexically_normal();
        const auto workers_value = parse_unsigned(argv[3], "maximum_workers");
        require(workers_value != 0u && workers_value <= 64u,
                "maximum_workers muss zwischen 1 und 64 liegen");
        const auto workers = static_cast<std::size_t>(workers_value);
        require(std::filesystem::is_directory(fixture_root),
                "Fixture-Root fehlt");
        require_cold_output(output_root);
        require(katana::analysis::global_analysis_executor().maximum_jobs() == workers,
                "KATANA_ANALYSIS_JOBS bindet nicht den geforderten Workeretat");

        std::mutex progress_events_mutex;
        std::vector<katana::ProgressEvent> progress_events;
        std::uint64_t last_structured_console_elapsed = 0u;
        const katana::ProgressReporter structured_reporter(
            [&](const katana::ProgressEvent& event) {
                const std::scoped_lock lock(progress_events_mutex);
                progress_events.push_back(event);
                if (event.state == katana::ProgressState::Heartbeat &&
                    (last_structured_console_elapsed == 0u ||
                     event.elapsed_milliseconds -
                             last_structured_console_elapsed >=
                         5'000u)) {
                    last_structured_console_elapsed =
                        event.elapsed_milliseconds;
                    std::cerr << "[heartbeat] "
                              << katana::progress_operation_name(
                                     event.operation)
                              << " completed=" << event.completed;
                    if (event.total) std::cerr << '/' << *event.total;
                    if (event.counters.active_workers)
                        std::cerr << " active="
                                  << *event.counters.active_workers;
                    if (event.counters.queued_work)
                        std::cerr << " queued="
                                  << *event.counters.queued_work;
                    std::cerr << '\n' << std::flush;
                }
            },
            std::chrono::milliseconds(0),
            std::chrono::milliseconds(100));

        stage("[1/11] GDI und deklarierte Analysegrenzen laden");
        const auto disc = katana::platform::load_dreamcast_gdi_boot(
            fixture_root / "disc.gdi");
        auto image = katana::platform::make_dreamcast_disc_executable(
            disc, katana::platform::DreamcastDiscExecutionPath::DirectBootFile);
        const auto overrides = katana::analysis::parse_analysis_overrides(
            fixture_root / "analysis.overrides");
        require(!overrides.functions.empty(),
                "Analyse-Overrides enthalten keine Funktionen");

        stage("[2/11] Echte Control-Flow-Analyse");
        const auto cfa_started = std::chrono::steady_clock::now();
        std::mutex cfa_progress_mutex;
        std::string cfa_phase;
        std::size_t cfa_iteration = std::numeric_limits<std::size_t>::max();
        SeedRoundLedger terminal_seed_round;
        katana::codegen::detail::StructuredControlFlowProgress
            structured_cfa_progress(structured_reporter, "kr4974-real-cfa");
        auto analysis = katana::analysis::analyze_control_flow(
            image,
            &overrides,
            [&](const katana::analysis::ControlFlowAnalysisProgress& progress) {
                const std::scoped_lock lock(cfa_progress_mutex);
                terminal_seed_round = seed_round_ledger(progress);
                structured_cfa_progress.update(progress);
                const std::array milestones{
                    std::string_view{"iteration-start"},
                    std::string_view{"recursive-complete"},
                    std::string_view{"local-resolution-complete"},
                    std::string_view{"function-values-start"},
                    std::string_view{"function-values-complete"},
                    std::string_view{"summary-seed-expansion"},
                    std::string_view{"fixpoint-complete"},
                    std::string_view{"complete"}};
                if (std::find(milestones.begin(), milestones.end(),
                              progress.phase) == milestones.end() ||
                    (progress.phase == cfa_phase &&
                     progress.iteration == cfa_iteration))
                    return;
                cfa_phase = std::string(progress.phase);
                cfa_iteration = progress.iteration;
                std::cerr << "[cfa] " << progress.phase
                          << " iteration=" << progress.iteration
                          << " seeds=" << progress.seeds
                          << " instructions=" << progress.instructions
                          << " summaries="
                          << progress.function_value_summarized_functions
                          << '/' << progress.function_value_functions
                          << " resolution="
                          << progress.function_value_resolution_functions_committed
                          << '/'
                          << progress.function_value_resolution_functions_total
                          << " workers=" << progress.function_value_active_workers
                          << '\n' << std::flush;
            },
            true);
        structured_cfa_progress.complete(analysis.fixpoint_iterations);
        const auto cfa_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cfa_started).count();
        auto ir_generation_progress = structured_reporter.begin(
            katana::ProgressOperation::IrGeneration,
            katana::ProgressUnit::Functions,
            overrides.functions.size(),
            "real-boot-ir-generation");
        auto program = katana::ir::lower_program(analysis);
        ir_generation_progress.complete(program.size());
        const auto& emission = katana::codegen::native_aot_emission_contract(
            katana::codegen::NativeAotEmissionProfile::Product);
        auto ir_optimization_progress = structured_reporter.begin(
            katana::ProgressOperation::IrOptimization,
            katana::ProgressUnit::Functions,
            program.size(),
            "real-boot-ir-optimization");
        static_cast<void>(katana::ir::optimize_program(
            program, emission.optimization_options));
        ir_optimization_progress.complete(program.size());
        require(program.size() == overrides.functions.size(),
                "echte Analyse/IR hat nicht jede Fixture-Funktion materialisiert");
        const auto profile = profile_for(program.size());

        stage("[3/11] Vollstaendigen Bootpartitionsplan validieren");
        katana::runtime::Iso9660Filesystem filesystem(
            disc.source, 2048u, disc.data_track_lba, disc.extent_lba_bias);
        const auto roots = parse_roots(filesystem.read_file("/ROOTS.BIN"));
        require(roots.records.size() == profile.root_count &&
                    roots.wave_count == profile.wave_count,
                "ROOTS.BIN passt nicht zum festen Profilvertrag");
        const katana::codegen::PartitionOptions partition_options{
            profile.function_count / profile.partition_count,
            std::numeric_limits<std::size_t>::max()};
        const auto boot_partitions = katana::codegen::partition_translation_units(
            program, partition_options);
        const auto expected_plan = canonical_partition_plan(
            profile, program, boot_partitions);
        const auto plan_bytes = filesystem.read_file("/PARTS.JSN");
        const std::string actual_plan(
            reinterpret_cast<const char*>(plan_bytes.data()), plan_bytes.size());
        require(actual_plan == expected_plan,
                "PARTS.JSN weicht vom vollstaendigen produktiven Bootplan ab");

        stage("[4/11] Reale wachsende FVA-Seedwellen plus exakte Replays");
        const auto fva_started = std::chrono::steady_clock::now();
        const auto fva = run_real_fva_waves(
            image, analysis, overrides, roots, program,
            profile.replay_passes, structured_reporter);
        const auto fva_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fva_started).count();

        stage("[5/11] Semantischen SCC-/ABI-/Ingress-Stress analysieren");
        const auto semantic_started = std::chrono::steady_clock::now();
        const auto semantic_bytes =
            filesystem.read_file("/SEMANTIC.BIN");
        const auto semantic = run_semantic_fva_stress(
            semantic_bytes, structured_reporter);
        const auto semantic_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - semantic_started).count();

        stage("[6/11] FVA-N-vs-8N-Gate mit spaetem Candidate-Seed");
        const auto delta_scale_started =
            std::chrono::steady_clock::now();
        const auto delta_scale =
            run_fva_delta_scale_gate(semantic_bytes);
        const auto delta_scale_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() -
            delta_scale_started).count();

        stage("[7/11] Produktions-CFA-N-vs-8N-Gate mit echtem Seed-Fixpunkt");
        const auto cfa_delta_scale_started =
            std::chrono::steady_clock::now();
        const auto cfa_delta_scale = run_cfa_delta_scale_gate();
        const auto cfa_delta_scale_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() -
            cfa_delta_scale_started).count();

        stage("[8/11] ExactOnly-Module, Entries und Bindings real analysieren");
        const auto latent_started = std::chrono::steady_clock::now();
        const auto hints = parse_latent_hints(
            read_regular_text(fixture_root / "latent-aot-entries.txt"));
        const auto exact = verify_exact_discovery(
            disc, hints, workers, structured_reporter);
        const auto latent_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - latent_started).count();

        stage("[9/11] Produktiven NativeDisc-Port exportieren");
        std::vector<katana::io::InputProvenance> inputs;
        const auto& descriptor = disc.source->descriptor();
        inputs.push_back({"gdi-descriptor", descriptor.size,
                          descriptor.sha256, descriptor.resolved_path});
        for (const auto& track : descriptor.tracks)
            inputs.push_back({"gdi-track-" + std::to_string(track.number),
                              track.file_offset +
                                  track.sector_count * track.sector_size,
                              track.sha256,
                              track.resolved_path});
        inputs.push_back(katana::io::capture_input_provenance(
            "analysis-overrides", fixture_root / "analysis.overrides"));
        const auto project_identity =
            katana::platform::dreamcast_disc_project_identity(disc);
        katana::codegen::PortExportOptions export_options;
        export_options.target_name = "katana_kr4974_stress";
        export_options.tool_version = "kr4974-stress-v3";
        export_options.partition_options = partition_options;
        export_options.diagnostic_partial = false;
        export_options.progress_callback = &export_progress;
        export_options.progress = structured_reporter;
        export_options.detailed_analysis_telemetry = true;
        export_options.latent_aot_entry_hints = hints;
        export_options.latent_aot_discovery_mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        const auto export_started = std::chrono::steady_clock::now();
        const auto report = katana::codegen::export_dreamcast_port_project(
            {image,
             analysis,
             program,
             inputs,
             katana::platform::dreamcast_disc_boot_address,
             katana::platform::dreamcast_disc_boot_address,
             disc.boot_file.size(),
             project_identity,
             true,
             true,
             disc.data_track_lba,
             disc.extent_lba_bias,
             true},
            output_root,
            export_options);
        const auto export_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - export_started).count();
        require(std::find(report.checkpoints.begin(), report.checkpoints.end(),
                          "latent-aot-registry-written") !=
                    report.checkpoints.end(),
                "Exact-Latent-AOT wurde nicht durch den realen Exportpfad verarbeitet");

        stage("[10/11] Reale Export-TUs, CMake-Quellen und Exact-Bindings pruefen");
        const auto metadata = read_port_metadata(output_root);
        require(json_unsigned(metadata, "latent_aot_modules") == exact.modules &&
                    json_unsigned(metadata, "latent_aot_source_bindings") ==
                        exact.source_bindings &&
                    json_unsigned(metadata, "function_count") == report.functions &&
                    report.functions >= program.size() + exact.entries,
                "Exportmetadaten verlieren ExactOnly-Module, Entries oder Bindings");
        const auto units = emitted_translation_units(output_root);
        require(!units.empty() && units.size() == report.partitions,
                "PortExportResult.partitions ist nicht an reale unit-v*.cpp gebunden");
        const auto generated_cmake = read_regular_text(
            output_root / "generated" / "CMakeLists.txt");
        require(count_occurrences(generated_cmake, "code/unit-v") == units.size(),
                "generiertes CMake referenziert nicht exakt alle realen TUs");
        for (const auto& unit : units)
            require(count_occurrences(
                        generated_cmake,
                        "code/" + unit.filename().generic_string()) == 1u,
                    "generiertes CMake verliert oder dupliziert eine reale TU");

        std::string dispatch_sources;
        for (const auto& entry : std::filesystem::directory_iterator(
                 output_root / "generated" / "code")) {
            const auto name = entry.path().filename().string();
            if (entry.path().extension() == ".cpp" &&
                !name.starts_with("unit-v"))
                dispatch_sources += read_regular_text(entry.path());
        }
        std::set<std::tuple<std::string, std::uint64_t, std::uint32_t>> bindings;
        for (const auto& hint : hints)
            bindings.emplace(
                hint.byte_identity, hint.disc_byte_offset, hint.byte_size);
        for (const auto& [identity, offset, size] : bindings) {
            const auto descriptor_fragment =
                std::to_string(offset) + "ull, " + std::to_string(size) +
                "u, \"" + identity + "\"});";
            require(count_occurrences(dispatch_sources, descriptor_fragment) == 1u,
                    "generierter Runtimebinder verliert eine Exact-Source-Bindung");
        }

        require(structured_reporter.seal_and_flush(),
                "realer Structured-Progress konnte nicht verlustfrei versiegelt werden");
        std::vector<katana::ProgressEvent> sealed_progress_events;
        {
            const std::scoped_lock lock(progress_events_mutex);
            sealed_progress_events = progress_events;
        }
        const auto progress_evidence = verify_structured_progress(
            std::move(sealed_progress_events),
            katana::codegen::configured_port_codegen_jobs(
                report.partitions),
            report.partitions,
            std::min(workers, exact.modules),
            exact.modules);

        stage("[11/11] Terminalen realen Stressvertrag ausgeben");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        auto combined_statistics = fva.statistics;
        combined_statistics.lookups += semantic.statistics.lookups;
        combined_statistics.ready_hits += semantic.statistics.ready_hits;
        combined_statistics.in_flight_coalesces +=
            semantic.statistics.in_flight_coalesces;
        combined_statistics.hits += semantic.statistics.hits;
        combined_statistics.misses += semantic.statistics.misses;
        combined_statistics.evictions += semantic.statistics.evictions;
        combined_statistics.entries += semantic.statistics.entries;
        combined_statistics.retained_payload_bytes +=
            semantic.statistics.retained_payload_bytes;
        for (std::size_t reason = 0u;
             reason < combined_statistics.miss_reasons.size(); ++reason)
            combined_statistics.miss_reasons[reason] +=
                semantic.statistics.miss_reasons[reason];
        require(combined_statistics.balanced(),
                "kombinierter echter Cacheledger ist unausgeglichen");
        const auto throughput_replay_fallbacks = std::accumulate(
            fva.waves.begin(), fva.waves.end(), std::size_t{0u},
            [](const auto total, const auto& wave) {
                return total + wave.cache_replay_fallback_recomputes;
            });
        const auto throughput_diagnostic_bypasses = std::accumulate(
            fva.waves.begin(), fva.waves.end(), std::size_t{0u},
            [](const auto total, const auto& wave) {
                return total + wave.cache_diagnostic_bypass_evaluations;
            });
        const auto combined_replay_fallbacks =
            throughput_replay_fallbacks +
            semantic.cache_replay_fallback_recomputes;
        const auto combined_diagnostic_bypasses =
            throughput_diagnostic_bypasses +
            semantic.cache_diagnostic_bypass_evaluations;
        const auto combined_logical_evaluations =
            fva.logical_evaluations + semantic.logical_evaluations;
        const auto combined_physical_evaluations =
            fva.physical_evaluations + semantic.physical_evaluations;
        const auto combined_multi_root_context_requests =
            fva.multi_root_context_requests +
            semantic.multi_root_context_requests;
        const auto combined_multi_root_unique_contexts =
            fva.multi_root_unique_contexts +
            semantic.multi_root_unique_contexts;
        const auto combined_multi_root_ready_reuses =
            fva.multi_root_ready_reuses + semantic.multi_root_ready_reuses;
        const auto combined_multi_root_in_flight_reuses =
            fva.multi_root_in_flight_reuses +
            semantic.multi_root_in_flight_reuses;
        const auto combined_multi_root_provenance_links =
            fva.multi_root_provenance_links +
            semantic.multi_root_provenance_links;
        const auto combined_multi_root_retained_contexts =
            fva.multi_root_retained_contexts +
            semantic.multi_root_retained_contexts;
        const auto combined_multi_root_retained_payload_bytes =
            fva.multi_root_retained_payload_bytes +
            semantic.multi_root_retained_payload_bytes;
        const auto combined_multi_root_evictions =
            fva.multi_root_evictions + semantic.multi_root_evictions;
        auto combined_incremental = fva.incremental;
        combined_incremental.add(semantic.incremental);
        auto combined_work = fva.work;
        combined_work.add(semantic.work);
        require(
            combined_logical_evaluations ==
                    combined_statistics.lookups +
                        combined_multi_root_ready_reuses +
                        combined_multi_root_in_flight_reuses +
                        combined_diagnostic_bypasses &&
                combined_physical_evaluations ==
                    combined_statistics.misses + combined_replay_fallbacks +
                        combined_diagnostic_bypasses &&
                combined_multi_root_context_requests ==
                    combined_multi_root_unique_contexts +
                        combined_multi_root_ready_reuses +
                        combined_multi_root_in_flight_reuses &&
                combined_multi_root_retained_contexts <=
                    combined_multi_root_unique_contexts &&
                combined_multi_root_evictions ==
                    combined_multi_root_unique_contexts -
                        combined_multi_root_retained_contexts &&
                ((combined_multi_root_retained_contexts == 0u) ==
                 (combined_multi_root_retained_payload_bytes == 0u)) &&
                combined_incremental.completed_without_fallback() &&
                combined_incremental.resolution_root_artifacts_total != 0u &&
                combined_diagnostic_bypasses == 0u,
            "kombinierter realer Multi-Root-/Cache-/Epochenledger ist "
            "unausgeglichen");
        constexpr std::array miss_reason_names{
            std::string_view{"cold"},
            std::string_view{"evicted"},
            std::string_view{"oversize_or_no_exact_replay"},
            std::string_view{"function_shape_changed"},
            std::string_view{"projected_ingress_changed"},
            std::string_view{"summary_dependency_changed"},
            std::string_view{"abi_contract_changed"},
            std::string_view{"resolution_lens_changed"},
            std::string_view{"inventory_sink_changed"},
            std::string_view{"isolation_partition_changed"},
            std::string_view{"contextual_summary_changed"},
            std::string_view{"tail_ingress_changed"},
        };
        std::ostringstream terminal;
        terminal << "{\"schema\":\"katana-native-disc-cold-build-stress-result-v3\""
                  << ",\"roots\":" << roots.records.size()
                  << ",\"seed_waves\":" << roots.wave_count
                  << ",\"fva_runs\":" << fva.runs
                  << ",\"replay_passes\":" << profile.replay_passes
                  << ",\"round_seed_facts_added\":"
                  << terminal_seed_round.seed_facts_added
                  << ",\"round_seed_targets_changed\":"
                  << terminal_seed_round.seed_targets_changed
                  << ",\"round_decode_targets\":"
                  << terminal_seed_round.decode_targets
                  << ",\"round_metadata_targets\":"
                  << terminal_seed_round.metadata_targets
                  << ",\"round_full_cpu_fallbacks\":"
                  << terminal_seed_round.full_cpu_fallbacks
                  << ",\"analysis_epochs_published\":"
                  << combined_incremental.analysis_epochs_published
                  << ",\"analysis_epochs_discarded\":"
                  << combined_incremental.analysis_epochs_discarded
                  << ",\"incremental_epochs_started\":"
                  << combined_incremental.incremental_epochs_started
                  << ",\"resolution_root_artifacts_total\":"
                  << combined_incremental.resolution_root_artifacts_total
                  << ",\"resolution_root_artifacts_reused\":"
                  << combined_incremental.resolution_root_artifacts_reused
                  << ",\"resolution_root_artifacts_recomputed\":"
                  << combined_incremental
                         .resolution_root_artifacts_recomputed
                  << ",\"resolution_root_artifacts_retained\":"
                  << combined_incremental.resolution_root_artifacts_retained
                  << ",\"resolution_epoch_retained_bytes\":"
                  << combined_incremental.resolution_epoch_retained_bytes
                  << ",\"resolution_retention_limit_reason\":\""
                  << katana::analysis::resolution_retention_limit_reason_name(
                         combined_incremental
                             .resolution_retention_limit_reason)
                  << "\""
                  << ",\"dirty_sccs\":"
                  << combined_incremental.dirty_sccs
                  << ",\"dirty_functions\":"
                  << combined_incremental.dirty_functions
                  << ",\"dirty_inventory_sinks\":"
                  << combined_incremental.dirty_inventory_sinks
                   << ",\"full_cpu_recompute_fallbacks\":"
                   << combined_incremental.full_cpu_recompute_fallbacks
                   << ",\"persistent_work\":";
        append_persistent_work_json(terminal, combined_work);
        terminal << ",\"logical_evaluations\":"
                  << combined_logical_evaluations
                  << ",\"cache_lookups\":" << combined_statistics.lookups
                  << ",\"cache_hits\":" << combined_statistics.hits
                  << ",\"cache_ready_hits\":" << combined_statistics.ready_hits
                  << ",\"cache_in_flight_coalesces\":"
                  << combined_statistics.in_flight_coalesces
                  << ",\"cache_misses\":" << combined_statistics.misses
                  << ",\"cache_evictions\":" << combined_statistics.evictions
                  << ",\"cache_entries\":" << combined_statistics.entries
                  << ",\"cache_retained_payload_bytes\":"
                  << combined_statistics.retained_payload_bytes
                  << ",\"physical_evaluations\":"
                  << combined_physical_evaluations
                  << ",\"cache_replay_fallback_recomputes\":"
                  << combined_replay_fallbacks
                  << ",\"cache_diagnostic_bypass_evaluations\":"
                  << combined_diagnostic_bypasses
                  << ",\"multi_root_context_requests\":"
                  << combined_multi_root_context_requests
                  << ",\"multi_root_unique_contexts\":"
                  << combined_multi_root_unique_contexts
                  << ",\"multi_root_ready_reuses\":"
                  << combined_multi_root_ready_reuses
                  << ",\"multi_root_in_flight_reuses\":"
                  << combined_multi_root_in_flight_reuses
                  << ",\"multi_root_provenance_links\":"
                  << combined_multi_root_provenance_links
                  << ",\"multi_root_retained_contexts\":"
                  << combined_multi_root_retained_contexts
                  << ",\"multi_root_retained_payload_bytes\":"
                  << combined_multi_root_retained_payload_bytes
                  << ",\"multi_root_evictions\":"
                  << combined_multi_root_evictions
                  << ",\"replay_hits\":" << fva.replay_hits
                  << ",\"replay_misses\":" << fva.replay_misses
                  << ",\"functions\":" << program.size()
                  << ",\"blocks\":" << block_count(program)
                  << ",\"boot_partitions\":" << boot_partitions.size()
                  << ",\"port_functions\":" << report.functions
                  << ",\"port_partitions\":" << report.partitions
                  << ",\"translation_units\":" << units.size()
                  << ",\"latent_modules\":" << exact.modules
                  << ",\"latent_entries\":" << exact.entries
                  << ",\"latent_source_bindings\":" << exact.source_bindings
                  << ",\"maximum_workers\":" << workers
                  << ",\"port_files\":" << report.generated_files
                  << ",\"structured_progress_events\":"
                  << progress_evidence.events
                  << ",\"structured_progress_max_gap_ms\":"
                  << progress_evidence.maximum_inter_record_gap_milliseconds
                  << ",\"structured_cfa_physical_work_ledger\":"
                  << (progress_evidence.cfa_physical_work_ledger
                          ? "true" : "false")
                  << ",\"structured_fva_persistent_work_ledger\":"
                  << (progress_evidence
                              .function_value_persistent_work_ledger
                          ? "true" : "false")
                  << ",\"source_configured_workers\":"
                  << progress_evidence.source_configured_workers
                  << ",\"latent_configured_workers\":"
                  << progress_evidence.latent_configured_workers
                  << ",\"semantic_targeted_misses\":"
                  << semantic.targeted_misses
                  << ",\"semantic_targeted_hits\":"
                  << semantic.targeted_hits
                  << ",\"semantic_targeted_incremental\":{"
                  << "\"analysis_epochs_published\":"
                  << semantic.targeted_incremental.analysis_epochs_published
                  << ",\"analysis_epochs_discarded\":"
                  << semantic.targeted_incremental.analysis_epochs_discarded
                  << ",\"incremental_epochs_started\":"
                  << semantic.targeted_incremental.incremental_epochs_started
                  << ",\"resolution_root_artifacts_total\":"
                  << semantic.targeted_incremental
                         .resolution_root_artifacts_total
                  << ",\"resolution_root_artifacts_reused\":"
                  << semantic.targeted_incremental
                         .resolution_root_artifacts_reused
                  << ",\"resolution_root_artifacts_recomputed\":"
                  << semantic.targeted_incremental
                         .resolution_root_artifacts_recomputed
                  << ",\"resolution_root_artifacts_retained\":"
                  << semantic.targeted_incremental
                         .resolution_root_artifacts_retained
                  << ",\"resolution_epoch_retained_bytes\":"
                  << semantic.targeted_incremental
                         .resolution_epoch_retained_bytes
                  << ",\"resolution_retention_limit_reason\":\""
                  << katana::analysis::resolution_retention_limit_reason_name(
                         semantic.targeted_incremental
                             .resolution_retention_limit_reason)
                  << "\""
                  << ",\"dirty_sccs\":"
                  << semantic.targeted_incremental.dirty_sccs
                  << ",\"dirty_functions\":"
                  << semantic.targeted_incremental.dirty_functions
                  << ",\"dirty_inventory_sinks\":"
                  << semantic.targeted_incremental.dirty_inventory_sinks
                  << ",\"full_cpu_recompute_fallbacks\":"
                   << semantic.targeted_incremental
                          .full_cpu_recompute_fallbacks
                   << "}"
                   << ",\"semantic_targeted_work\":";
        append_persistent_work_json(
            terminal, semantic.targeted_work);
        terminal << ",\"semantic_replay_hits\":"
                  << semantic.replay_hits
                  << ",\"semantic_sccs\":"
                  << semantic.strongly_connected_components
                  << ",\"semantic_hol_ms\":"
                  << semantic.maximum_head_of_line_milliseconds
                  << ",\"semantic_ready_ahead\":"
                  << semantic.maximum_ready_ahead
                  << ",\"semantic_invalidation_baseline_version\":1"
                  << ",\"semantic_targeted_miss_functions\":[";
        for (std::size_t index = 0u;
             index < semantic.targeted_miss_functions.size(); ++index) {
            if (index != 0u) terminal << ',';
            terminal << semantic.targeted_miss_functions[index];
        }
        terminal << "],\"semantic_targeted_hit_only_functions\":[";
        for (std::size_t index = 0u;
             index < semantic.targeted_hit_only_functions.size(); ++index) {
            if (index != 0u) terminal << ',';
            terminal << semantic.targeted_hit_only_functions[index];
        }
         terminal << "]"
                   << ",\"fva_delta_scale_gate\":{\"n\":";
        append_fva_delta_scale_sample_json(terminal, delta_scale.n);
        terminal << ",\"eight_n\":";
        append_fva_delta_scale_sample_json(
            terminal, delta_scale.eight_n);
        terminal << "}"
                   << ",\"cfa_delta_scale_gate\":{\"n\":";
        append_cfa_delta_scale_sample_json(
            terminal, cfa_delta_scale.n);
        terminal << ",\"eight_n\":";
        append_cfa_delta_scale_sample_json(
            terminal, cfa_delta_scale.eight_n);
        terminal << "}"
                   << ",\"timings_ms\":{\"cfa\":" << cfa_elapsed
                   << ",\"fva_waves\":" << fva_elapsed
                   << ",\"semantic_fva\":" << semantic_elapsed
                   << ",\"fva_delta_scale\":"
                   << delta_scale_elapsed
                   << ",\"cfa_delta_scale\":"
                   << cfa_delta_scale_elapsed
                   << ",\"latent\":" << latent_elapsed
                  << ",\"export\":" << export_elapsed << "}"
                  << ",\"component_suite_elapsed_ms\":" << elapsed;
        terminal << ",\"miss_reasons\":{";
        for (std::size_t reason = 0u; reason < miss_reason_names.size(); ++reason) {
            if (reason != 0u) terminal << ',';
            terminal << '\"' << miss_reason_names[reason] << "\":"
                     << combined_statistics.miss_reasons[reason];
        }
        terminal << "},\"semantic_targeted_miss_reasons\":{";
        for (std::size_t reason = 0u; reason < miss_reason_names.size(); ++reason) {
            if (reason != 0u) terminal << ',';
            terminal << '\"' << miss_reason_names[reason] << "\":"
                     << semantic.targeted_miss_reasons[reason];
        }
        terminal << "},\"fva_wave_ledger\":[";
        for (std::size_t wave = 0u; wave < fva.waves.size(); ++wave) {
            const auto& ledger = fva.waves[wave];
            if (wave != 0u) terminal << ',';
            terminal << "{\"index\":" << ledger.index
                     << ",\"replay\":" << (ledger.replay ? "true" : "false")
                     << ",\"added_roots\":" << ledger.added_roots
                     << ",\"boundaries\":" << ledger.boundaries
                     << ",\"summaries\":" << ledger.summaries
                     << ",\"logical_evaluations\":"
                     << ledger.logical_evaluations
                     << ",\"physical_evaluations\":"
                     << ledger.physical_evaluations
                     << ",\"cache_lookups\":" << ledger.cache_lookups
                     << ",\"cache_ready_hits\":" << ledger.cache_ready_hits
                     << ",\"cache_in_flight_coalesces\":"
                     << ledger.cache_in_flight_coalesces
                     << ",\"cache_hits\":" << ledger.cache_hits
                     << ",\"cache_misses\":" << ledger.cache_misses
                     << ",\"cache_replay_fallback_recomputes\":"
                     << ledger.cache_replay_fallback_recomputes
                     << ",\"cache_diagnostic_bypass_evaluations\":"
                     << ledger.cache_diagnostic_bypass_evaluations
                     << ",\"multi_root_context_requests\":"
                     << ledger.multi_root_context_requests
                     << ",\"multi_root_unique_contexts\":"
                     << ledger.multi_root_unique_contexts
                     << ",\"multi_root_ready_reuses\":"
                     << ledger.multi_root_ready_reuses
                     << ",\"multi_root_in_flight_reuses\":"
                     << ledger.multi_root_in_flight_reuses
                     << ",\"multi_root_provenance_links\":"
                     << ledger.multi_root_provenance_links
                     << ",\"multi_root_retained_contexts\":"
                     << ledger.multi_root_retained_contexts
                     << ",\"multi_root_retained_payload_bytes\":"
                     << ledger.multi_root_retained_payload_bytes
                     << ",\"multi_root_evictions\":"
                     << ledger.multi_root_evictions
                     << ",\"analysis_epochs_published\":"
                     << ledger.incremental.analysis_epochs_published
                     << ",\"analysis_epochs_discarded\":"
                     << ledger.incremental.analysis_epochs_discarded
                     << ",\"incremental_epochs_started\":"
                     << ledger.incremental.incremental_epochs_started
                     << ",\"resolution_root_artifacts_total\":"
                     << ledger.incremental.resolution_root_artifacts_total
                     << ",\"resolution_root_artifacts_reused\":"
                     << ledger.incremental.resolution_root_artifacts_reused
                     << ",\"resolution_root_artifacts_recomputed\":"
                     << ledger.incremental
                            .resolution_root_artifacts_recomputed
                     << ",\"resolution_root_artifacts_retained\":"
                     << ledger.incremental
                            .resolution_root_artifacts_retained
                     << ",\"resolution_epoch_retained_bytes\":"
                     << ledger.incremental.resolution_epoch_retained_bytes
                     << ",\"resolution_retention_limit_reason\":\""
                     << katana::analysis::resolution_retention_limit_reason_name(
                            ledger.incremental
                                .resolution_retention_limit_reason)
                     << "\""
                     << ",\"dirty_sccs\":"
                     << ledger.incremental.dirty_sccs
                     << ",\"dirty_functions\":"
                     << ledger.incremental.dirty_functions
                     << ",\"dirty_inventory_sinks\":"
                     << ledger.incremental.dirty_inventory_sinks
                      << ",\"full_cpu_recompute_fallbacks\":"
                      << ledger.incremental.full_cpu_recompute_fallbacks
                      << ",\"persistent_work\":";
            append_persistent_work_json(terminal, ledger.work);
            terminal << ",\"miss_reasons\":{";
            for (std::size_t reason = 0u;
                 reason < miss_reason_names.size(); ++reason) {
                if (reason != 0u) terminal << ',';
                terminal << '\"' << miss_reason_names[reason] << "\":"
                         << ledger.miss_reasons[reason];
            }
            terminal << "}}";
        }
        terminal << "]}\n";
        std::cout << terminal.str();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
