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

constexpr std::uint32_t stress_schema_version = 2u;
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
    require(read_u32(bytes, 8u) == stress_schema_version,
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
    RealFvaStressResult aggregate;

    const auto run = [&](const std::size_t index,
                         const bool replay,
                         const std::size_t added_roots) {
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
                        counters.added_work = added_roots;
                        counters.growing_workset =
                            !replay && index != 0u && added_roots != 0u;
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
        std::set<std::uint32_t> boundary_addresses;
        std::set<std::uint32_t> summary_addresses;
        for (const auto& boundary : boundaries)
            boundary_addresses.insert(boundary.entry_address);
        for (const auto& summary : result.summaries)
            summary_addresses.insert(summary.function_address);
        require(boundary_addresses.size() == boundaries.size() &&
                    summary_addresses.size() == result.summaries.size() &&
                    boundary_addresses == summary_addresses,
                "reale FVA-Summary-Adressen entsprechen nicht exakt den Boundaries");
        const auto after = session.statistics();
        require(after.lookups >= before.lookups &&
                    after.hits >= before.hits && after.misses >= before.misses,
                "FunctionValue-Sessionzaehler liefen rueckwaerts");
        const auto lookup_delta = after.lookups - before.lookups;
        const auto hit_delta = after.hits - before.hits;
        const auto miss_delta = after.misses - before.misses;
        require(last_progress.phase == "complete" &&
                    last_progress.functions == boundaries.size() &&
                    last_progress.summarized_functions == boundaries.size() &&
                    last_progress.logical_evaluations == lookup_delta &&
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
                    last_progress.cache_diagnostic_bypass_evaluations == 0u,
                "reale FVA-Progress-/Session-/Physical-Bilanz driftet");
        RealFvaWaveLedger ledger;
        ledger.index = index;
        ledger.added_roots = added_roots;
        ledger.boundaries = boundaries.size();
        ledger.summaries = result.summaries.size();
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
        for (std::size_t reason = 0u; reason < ledger.miss_reasons.size(); ++reason)
            ledger.miss_reasons[reason] =
                after.miss_reasons[reason] - before.miss_reasons[reason];
        require(std::accumulate(ledger.miss_reasons.begin(),
                                ledger.miss_reasons.end(), std::size_t{0u}) ==
                    miss_delta,
                "FVA-Welle verlor ihre primaere Missgrundbilanz");
        ledger.replay = replay;
        if (!replay) {
            require(added_roots != 0u && miss_delta != 0u,
                    "wachsende FVA-Welle erzeugte keine echten neuen Misses");
            if (index != 0u)
                require(hit_delta != 0u,
                        "wachsende FVA-Welle traf die unveraenderte Altmenge nicht");
        } else {
            require(added_roots == 0u && lookup_delta == hit_delta &&
                        hit_delta != 0u && miss_delta == 0u &&
                        last_progress.physical_evaluations == 0u &&
                        last_progress.cache_replay_fallback_recomputes == 0u &&
                        last_progress.cache_diagnostic_bypass_evaluations == 0u,
                    "identischer Replay war nicht vollstaendig physisch arbeitsfrei");
        }
        aggregate.waves.push_back(ledger);
        aggregate.logical_evaluations += last_progress.logical_evaluations;
        aggregate.physical_evaluations += last_progress.physical_evaluations;
        ++aggregate.runs;
        if (replay) {
            aggregate.replay_hits = hit_delta;
            aggregate.replay_misses = miss_delta;
        }
        structured_progress.update(index + 1u);
    };

    for (std::size_t wave = 0u; wave < roots.wave_count; ++wave) {
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
            boundaries.push_back({declaration.address, declaration.size});
        }
        std::sort(boundaries.begin(), boundaries.end(), [](const auto& left,
                                                           const auto& right) {
            return left.entry_address < right.entry_address;
        });
        run(wave, false, std::count_if(
            roots.records.begin(), roots.records.end(),
            [&](const auto& root) { return root.wave == wave; }));
    }
    require(boundaries.size() == roots.records.size(),
            "Seedwellen materialisierten nicht alle Roots");
    require(replay_passes != 0u, "FVA-Stress besitzt keinen exakten Replay");
    for (std::size_t replay = 0u; replay < replay_passes; ++replay)
        run(roots.wave_count + replay, true, 0u);

    structured_progress.complete(total_runs);
    aggregate.statistics = session.statistics();
    require(aggregate.statistics.balanced() &&
                aggregate.statistics.misses == aggregate.physical_evaluations &&
                aggregate.statistics.hits != 0u &&
                aggregate.statistics.misses != 0u &&
                aggregate.replay_hits != 0u &&
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
        {semantic_base + 0x5100u, 0x04u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 5u>
        baseline_edges{{
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
        katana::ProgressCounterSnapshot initial_counters;
        initial_counters.pass = run_index + 1u;
        initial_counters.planned_work = boundaries.size();
        initial_counters.configured_workers =
            katana::analysis::global_analysis_executor().maximum_jobs();
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
                        aggregate.maximum_head_of_line_milliseconds = std::max(
                            aggregate.maximum_head_of_line_milliseconds,
                            progress.resolution_head_of_line_elapsed_milliseconds);
                        if (progress.resolution_functions_ready >=
                            progress.resolution_functions_committed)
                            aggregate.maximum_ready_ahead = std::max(
                                aggregate.maximum_ready_ahead,
                                progress.resolution_functions_ready -
                                    progress.resolution_functions_committed);
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
                        counters.head_of_line_index =
                            progress.resolution_head_of_line_index;
                        counters.head_of_line_elapsed_milliseconds =
                            progress.resolution_head_of_line_elapsed_milliseconds;
                        counters.ready_ahead =
                            progress.resolution_functions_ready -
                            std::min(progress.resolution_functions_ready,
                                     progress.resolution_functions_committed);
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
        require(!result.budget_exhausted && terminal_phase == "complete" &&
                    terminal_functions == boundaries.size() &&
                    terminal_summaries == boundaries.size(),
                "semantische FVA erreichte keinen exakten Terminalzustand");
        std::set<std::uint32_t> summaries;
        for (const auto& summary : result.summaries)
            summaries.insert(summary.function_address);
        std::set<std::uint32_t> expected;
        for (const auto& boundary : boundaries)
            expected.insert(boundary.entry_address);
        require(summaries == expected &&
                    result.summaries.size() == boundaries.size(),
                "semantische FVA verlor oder erfand eine Summary");
        aggregate.strongly_connected_components =
            result.strongly_connected_components;
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
        require(terminal_logical == lookups &&
                    terminal_physical == misses + terminal_fallbacks +
                        terminal_diagnostic_bypasses &&
                    terminal_diagnostic_bypasses == 0u,
                "semantische FVA-Cache-/Physical-Bilanz driftet");
        if (exact_replay)
            require(lookups == hits && hits != 0u && misses == 0u &&
                        terminal_physical == 0u && terminal_fallbacks == 0u,
                    "semantischer exakter Replay war nicht physisch arbeitsfrei");
        if (targeted_change) {
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
        aggregate.cache_replay_fallback_recomputes += terminal_fallbacks;
        aggregate.cache_diagnostic_bypass_evaluations +=
            terminal_diagnostic_bypasses;
        if (exact_replay) aggregate.replay_hits += hits;
        semantic_progress.update(run_index + 1u);
    };

    run(baseline_edges, 0u, false, false);
    run(baseline_edges, 1u, true, false);
    run(changed_edges, 2u, false, true);
    run(baseline_edges, 3u, true, false);
    semantic_progress.complete(4u);
    aggregate.statistics = session.statistics();
    const auto noncold_targeted = std::accumulate(
        aggregate.targeted_miss_reasons.begin() + 1,
        aggregate.targeted_miss_reasons.end(), std::size_t{0u});
    constexpr std::array<std::size_t,
                         katana::analysis::detail::
                             function_evaluation_cache_miss_reason_count>
        expected_targeted_miss_closure{
            0u, 0u, 0u, 1u, 17u, 2u, 0u, 1u, 0u, 0u, 0u, 0u};
    const std::vector<std::uint32_t> expected_targeted_miss_functions{
        semantic_base,
        semantic_base + 0x5000u,
        semantic_base + 0x5040u,
        semantic_base + 0x5080u,
        semantic_base + 0x50C0u};
    const std::vector<std::uint32_t> expected_targeted_hit_only_functions{
        semantic_base + 0x5100u};
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
                aggregate.targeted_misses == 21u &&
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
    bool growing_workset_observed = false;
    bool head_of_line_observed = false;
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
        if (event.operation == katana::ProgressOperation::FunctionValueAnalysis) {
            const auto& counters = event.counters;
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
                        *counters.cache_diagnostic_bypass_evaluations;
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
            if (ledger_balanced &&
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
    evidence.all_required_operations_terminal = std::all_of(
        required_operations.begin(), required_operations.end(),
        [&](const auto operation) {
            return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.operation == operation &&
                       event.state == katana::ProgressState::Completed;
            });
        });
    const std::array required_subphases{
        std::string_view{"inventory-region-closure"},
        std::string_view{"abi-return-signatures"},
        std::string_view{"abi-stack-reads"},
        std::string_view{"abi-register-reads"},
        std::string_view{"persistent-store-signatures"},
        std::string_view{"inventory-reachability"},
        std::string_view{"cache-key-plan"},
    };
    evidence.all_function_subphases_terminal = std::all_of(
        required_subphases.begin(), required_subphases.end(),
        [&](const auto label) {
            return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.label == label &&
                       event.state == katana::ProgressState::Completed;
            });
        });
    const auto complete =
        evidence.source_configured_workers == expected_source_workers &&
        evidence.latent_configured_workers == expected_latent_workers &&
        evidence.function_value_cache_ledger &&
        evidence.function_value_physical_work &&
        evidence.growing_workset_observed &&
        evidence.head_of_line_observed &&
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
                << " growing=" << evidence.growing_workset_observed
                << " hol=" << evidence.head_of_line_observed
                << " operations="
                << evidence.all_required_operations_terminal
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

        stage("[1/9] GDI und deklarierte Analysegrenzen laden");
        const auto disc = katana::platform::load_dreamcast_gdi_boot(
            fixture_root / "disc.gdi");
        auto image = katana::platform::make_dreamcast_disc_executable(
            disc, katana::platform::DreamcastDiscExecutionPath::DirectBootFile);
        const auto overrides = katana::analysis::parse_analysis_overrides(
            fixture_root / "analysis.overrides");
        require(!overrides.functions.empty(),
                "Analyse-Overrides enthalten keine Funktionen");

        stage("[2/9] Echte Control-Flow-Analyse");
        const auto cfa_started = std::chrono::steady_clock::now();
        std::mutex cfa_progress_mutex;
        std::string cfa_phase;
        std::size_t cfa_iteration = std::numeric_limits<std::size_t>::max();
        katana::codegen::detail::StructuredControlFlowProgress
            structured_cfa_progress(structured_reporter, "kr4974-real-cfa");
        auto analysis = katana::analysis::analyze_control_flow(
            image,
            &overrides,
            [&](const katana::analysis::ControlFlowAnalysisProgress& progress) {
                const std::scoped_lock lock(cfa_progress_mutex);
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

        stage("[3/9] Vollstaendigen Bootpartitionsplan validieren");
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

        stage("[4/9] Reale wachsende FVA-Seedwellen plus exakte Replays");
        const auto fva_started = std::chrono::steady_clock::now();
        const auto fva = run_real_fva_waves(
            image, analysis, overrides, roots, program,
            profile.replay_passes, structured_reporter);
        const auto fva_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fva_started).count();

        stage("[5/9] Semantischen SCC-/ABI-/Ingress-Stress analysieren");
        const auto semantic_started = std::chrono::steady_clock::now();
        const auto semantic = run_semantic_fva_stress(
            filesystem.read_file("/SEMANTIC.BIN"), structured_reporter);
        const auto semantic_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - semantic_started).count();

        stage("[6/9] ExactOnly-Module, Entries und Bindings real analysieren");
        const auto latent_started = std::chrono::steady_clock::now();
        const auto hints = parse_latent_hints(
            read_regular_text(fixture_root / "latent-aot-entries.txt"));
        const auto exact = verify_exact_discovery(
            disc, hints, workers, structured_reporter);
        const auto latent_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - latent_started).count();

        stage("[7/9] Produktiven NativeDisc-Port exportieren");
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
        export_options.tool_version = "kr4974-stress-v2";
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

        stage("[8/9] Reale Export-TUs, CMake-Quellen und Exact-Bindings pruefen");
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

        stage("[9/9] Terminalen realen Stressvertrag ausgeben");
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
        require(combined_diagnostic_bypasses == 0u,
                "Produktstress lief unerwartet im Diagnose-Bypassmodus");
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
        terminal << "{\"schema\":\"katana-native-disc-cold-build-stress-result-v2\""
                  << ",\"roots\":" << roots.records.size()
                  << ",\"seed_waves\":" << roots.wave_count
                  << ",\"fva_runs\":" << fva.runs
                  << ",\"replay_passes\":" << profile.replay_passes
                  << ",\"logical_evaluations\":"
                  << fva.logical_evaluations + semantic.logical_evaluations
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
                  << fva.physical_evaluations + semantic.physical_evaluations
                  << ",\"cache_replay_fallback_recomputes\":"
                  << combined_replay_fallbacks
                  << ",\"cache_diagnostic_bypass_evaluations\":"
                  << combined_diagnostic_bypasses
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
                  << ",\"source_configured_workers\":"
                  << progress_evidence.source_configured_workers
                  << ",\"latent_configured_workers\":"
                  << progress_evidence.latent_configured_workers
                  << ",\"semantic_targeted_misses\":"
                  << semantic.targeted_misses
                  << ",\"semantic_targeted_hits\":"
                  << semantic.targeted_hits
                  << ",\"semantic_replay_hits\":"
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
                  << ",\"timings_ms\":{\"cfa\":" << cfa_elapsed
                  << ",\"fva_waves\":" << fva_elapsed
                  << ",\"semantic_fva\":" << semantic_elapsed
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
                     << ",\"miss_reasons\":{";
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
