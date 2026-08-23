#include "katana/analysis/control_flow_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/instruction.hpp"
#include "guarded_native_entry_shape.hpp"
#include "jump_table_analysis_internal.hpp"
#include "static_callback_inventory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace katana::analysis {
namespace {

constexpr std::size_t maximum_function_value_candidate_contract_iterations =
    64u;

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
    return output.str();
}

[[noreturn]] void override_error(const AnalysisOverrides& overrides,
                                 const std::size_t line,
                                 const std::uint32_t address,
                                 const std::string& cause) {
    throw std::runtime_error("Analyseanweisungsfehler in " + overrides.source_path.string() +
                             " in Zeile " + std::to_string(line) + " bei " + hex_address(address) +
                             ": " + cause + ".");
}

void require_override_code_address(const katana::io::ExecutableImage& image,
                                   const AnalysisOverrides& overrides,
                                   const std::size_t line,
                                   const std::uint32_t address) {
    const auto validation = validate_committed_code_address(image, address);
    if (!validation.valid()) {
        override_error(overrides, line, address, code_address_status_name(validation.status));
    }
}

struct SeedEvidence {
    std::set<FunctionOrigin> origins;
    bool proven = false;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::uint32_t function_size = 0u;
    std::vector<ControlFlowAnalysisResult::SeedCause> causes;
};

struct ExactFunctionOwnershipInterval final {
    std::uint32_t begin = 0u;
    std::uint64_t end = 0u;
};

[[nodiscard]] bool exact_function_strictly_contains(
    const std::span<const ExactFunctionOwnershipInterval> intervals,
    const std::uint32_t address) noexcept {
    const auto candidate = std::upper_bound(
        intervals.begin(), intervals.end(), address,
        [](const auto value, const auto& interval) {
            return value < interval.begin;
        });
    if (candidate == intervals.begin()) return false;
    const auto& interval = *std::prev(candidate);
    return address > interval.begin &&
           static_cast<std::uint64_t>(address) < interval.end;
}

[[nodiscard]] std::uint32_t exact_function_size_at(
    const std::span<const ExactFunctionOwnershipInterval> intervals,
    const std::uint32_t address) noexcept {
    const auto candidate = std::lower_bound(
        intervals.begin(), intervals.end(), address,
        [](const auto& interval, const auto value) {
            return interval.begin < value;
        });
    if (candidate == intervals.end() || candidate->begin != address)
        return 0u;
    const auto size = candidate->end - candidate->begin;
    return size <= std::numeric_limits<std::uint32_t>::max()
               ? static_cast<std::uint32_t>(size)
               : 0u;
}

using SeedCause = ControlFlowAnalysisResult::SeedCause;
using SeedCauseKind = ControlFlowAnalysisResult::SeedCauseKind;

[[nodiscard]] bool seed_cause_less(const SeedCause& left,
                                   const SeedCause& right) noexcept {
    return std::tie(left.kind,
                    left.source_address,
                    left.source_object,
                    left.owner_address,
                    left.evidence_call_sites,
                    left.evidence_callees) <
           std::tie(right.kind,
                    right.source_address,
                    right.source_object,
                    right.owner_address,
                    right.evidence_call_sites,
                    right.evidence_callees);
}

struct SeedChangeTracker {
    std::set<std::uint32_t> changed_targets;
    std::set<std::uint32_t> decode_targets;
    std::set<std::uint32_t> metadata_targets;
    std::size_t facts_added = 0u;
    bool exact_boundary_changed = false;

    [[nodiscard]] bool analysis_changed() const noexcept {
        return !decode_targets.empty() ||
               !metadata_targets.empty() ||
               exact_boundary_changed;
    }
};

struct SeedLedgerTelemetry {
    std::size_t targets_added = 0u;
    std::size_t targets_strengthened = 0u;
    std::size_t causes_added = 0u;
    std::set<std::uint32_t> decode_targets;
    std::set<std::uint32_t> metadata_targets;
};

bool add_seed(std::map<std::uint32_t, SeedEvidence>& seeds,
              const std::uint32_t address,
              const std::span<const FunctionOrigin> origins = {},
              const bool proven = true,
              const ControlFlowEvidence evidence =
                  ControlFlowEvidence::ProvenComplete,
              const std::uint32_t function_size = 0u,
              const std::optional<SeedCause> cause = std::nullopt,
              SeedChangeTracker* const changes = nullptr,
              SeedLedgerTelemetry* const telemetry = nullptr) {
    const auto [iterator, inserted] = seeds.try_emplace(address);
    bool evidence_strengthened = false;
    bool metadata_changed = false;
    bool exact_boundary_changed = false;
    if (function_size != 0u) {
        if (iterator->second.function_size != 0u &&
            iterator->second.function_size != function_size)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen widersprechen sich.");
        if (iterator->second.function_size == 0u) {
            iterator->second.function_size = function_size;
            metadata_changed = true;
            exact_boundary_changed = true;
        }
    }
    if (proven && !iterator->second.proven) {
        iterator->second.proven = true;
        evidence_strengthened = true;
    }
    if (control_flow_evidence_preferred_for_static_decode(evidence,
                                                         iterator->second.evidence)) {
        evidence_strengthened =
            iterator->second.evidence != evidence ||
            evidence_strengthened;
        iterator->second.evidence = evidence;
    }
    for (const auto origin : origins) {
        metadata_changed =
            iterator->second.origins.insert(origin).second ||
            metadata_changed;
    }
    bool cause_added = false;
    if (cause.has_value()) {
        const auto position = std::lower_bound(
            iterator->second.causes.begin(),
            iterator->second.causes.end(),
            *cause,
            seed_cause_less);
        if (position == iterator->second.causes.end() ||
            *position != *cause) {
            iterator->second.causes.insert(position, *cause);
            cause_added = true;
        }
    }
    const bool decode_changed = inserted || evidence_strengthened;
    const bool changed = decode_changed || metadata_changed;
    if (changes != nullptr) {
        if (changed) changes->changed_targets.insert(address);
        if (decode_changed) changes->decode_targets.insert(address);
        if (metadata_changed)
            changes->metadata_targets.insert(address);
        changes->exact_boundary_changed =
            changes->exact_boundary_changed ||
            exact_boundary_changed;
        if (cause_added) ++changes->facts_added;
    }
    if (telemetry != nullptr) {
        if (inserted) ++telemetry->targets_added;
        if (!inserted && evidence_strengthened)
            ++telemetry->targets_strengthened;
        if (cause_added) ++telemetry->causes_added;
        if (decode_changed)
            telemetry->decode_targets.insert(address);
        if (metadata_changed)
            telemetry->metadata_targets.insert(address);
    }
    return changed;
}

bool add_resolution_seeds(std::map<std::uint32_t, SeedEvidence>& seeds,
                          const IndirectControlFlowResolution& resolution,
                          SeedChangeTracker* const changes = nullptr,
                          SeedLedgerTelemetry* const telemetry = nullptr) {
    auto targets = resolution.targets;
    if (resolution.target.has_value()) targets.push_back(*resolution.target);
    targets.insert(targets.end(),
                   resolution.analysis_candidates.begin(),
                   resolution.analysis_candidates.end());
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.empty()) return false;
    bool changed = false;
    const bool relative_call_island =
        resolution.reason == "runtime-contract-snapshot-relative-call-island-candidates";
    auto evidence_call_sites = resolution.evidence_call_sites;
    auto evidence_callees = resolution.evidence_callees;
    std::sort(evidence_call_sites.begin(), evidence_call_sites.end());
    evidence_call_sites.erase(
        std::unique(evidence_call_sites.begin(),
                    evidence_call_sites.end()),
        evidence_call_sites.end());
    std::sort(evidence_callees.begin(), evidence_callees.end());
    evidence_callees.erase(
        std::unique(evidence_callees.begin(), evidence_callees.end()),
        evidence_callees.end());
    for (const auto target : targets) {
        const auto analysis_candidate =
            std::find(resolution.analysis_candidates.begin(),
                      resolution.analysis_candidates.end(),
                      target) != resolution.analysis_candidates.end();
        const auto resolved_target =
            resolution.target == target ||
            std::find(resolution.targets.begin(),
                      resolution.targets.end(),
                      target) != resolution.targets.end();
        std::vector<SeedCauseKind> cause_kinds;
        if (relative_call_island || analysis_candidate)
            cause_kinds.push_back(
                SeedCauseKind::IndirectAnalysisCandidate);
        if (!relative_call_island && resolved_target)
            cause_kinds.push_back(
                SeedCauseKind::IndirectControlFlowTarget);
        std::sort(cause_kinds.begin(), cause_kinds.end());
        cause_kinds.erase(
            std::unique(cause_kinds.begin(), cause_kinds.end()),
            cause_kinds.end());
        for (const auto cause_kind : cause_kinds) {
            SeedCause cause{
                cause_kind,
                resolution.instruction_address,
                std::nullopt,
                std::nullopt};
            cause.evidence_call_sites = evidence_call_sites;
            cause.evidence_callees = evidence_callees;
            if (relative_call_island) {
                changed = add_seed(seeds,
                                   target,
                                   {},
                                   false,
                                   ControlFlowEvidence::GuardedPartial,
                                   0u,
                                   cause,
                                   changes,
                                   telemetry) ||
                          changed;
                continue;
            }
            if (resolution.kind != IndirectControlFlowKind::Call) {
                changed = add_seed(
                              seeds,
                              target,
                              {},
                              control_flow_evidence_proven(
                                  resolution.evidence),
                              resolution.evidence,
                              0u,
                              cause,
                              changes,
                              telemetry) ||
                          changed;
                continue;
            }
            if (resolution.reason == "user-override" ||
                resolution.reason == "user-hint") {
                const std::array origins{
                    FunctionOrigin::IndirectCall,
                    resolution.reason == "user-hint"
                        ? FunctionOrigin::UserHint
                        : FunctionOrigin::UserOverride};
                changed = add_seed(seeds,
                                   target,
                                   origins,
                                   false,
                                   resolution.evidence,
                                   0u,
                                   cause,
                                   changes,
                                   telemetry) ||
                          changed;
            } else if (resolution.status ==
                       ResolutionStatus::Guarded) {
                const std::array origins{
                    FunctionOrigin::GuardedSnapshot};
                changed = add_seed(seeds,
                                   target,
                                   origins,
                                   false,
                                   resolution.evidence,
                                   0u,
                                   cause,
                                   changes,
                                   telemetry) ||
                          changed;
            } else {
                const std::array origins{
                    FunctionOrigin::IndirectCall};
                changed = add_seed(
                              seeds,
                              target,
                              origins,
                              control_flow_evidence_proven(
                                  resolution.evidence),
                              resolution.evidence,
                              0u,
                              cause,
                              changes,
                              telemetry) ||
                          changed;
            }
        }
    }
    return changed;
}

[[nodiscard]] AnalysisSeed make_analysis_seed(
    const std::uint32_t address,
    const SeedEvidence& evidence,
    const std::span<const ExactFunctionOwnershipInterval>
        exact_function_ownership) {
    AnalysisSeed seed;
    seed.address = address;
    if (!exact_function_strictly_contains(
            exact_function_ownership, address))
        seed.function_origins.assign(evidence.origins.begin(),
                                     evidence.origins.end());
    seed.guarded_candidate = !evidence.proven;
    seed.evidence = evidence.evidence;
    const auto exact_size = exact_function_size_at(
        exact_function_ownership, address);
    seed.function_size = exact_size != 0u ? exact_size
                                         : evidence.function_size;
    return seed;
}

std::vector<AnalysisSeed> make_seed_vector(
    const std::map<std::uint32_t, SeedEvidence>& seeds,
    const std::span<const ExactFunctionOwnershipInterval>
        exact_function_ownership,
    const std::set<std::uint32_t>* const selected_targets =
        nullptr) {
    std::vector<AnalysisSeed> result;
    result.reserve(selected_targets == nullptr ? seeds.size()
                                               : selected_targets->size());
    if (selected_targets != nullptr) {
        for (const auto address : *selected_targets) {
            const auto found = seeds.find(address);
            if (found == seeds.end()) continue;
            result.push_back(
                make_analysis_seed(found->first, found->second,
                                   exact_function_ownership));
        }
        return result;
    }
    for (const auto& [address, evidence] : seeds)
        result.push_back(make_analysis_seed(
            address, evidence, exact_function_ownership));
    return result;
}

struct RecursiveWorkingIndex final {
    std::map<std::uint32_t, katana::sh4::DisassemblyLine> lines;
    std::map<std::uint32_t, FunctionCandidate> functions;
    std::set<std::uint32_t> proven_instruction_addresses;

    void clear() {
        lines.clear();
        functions.clear();
        proven_instruction_addresses.clear();
    }

    void apply(const detail::RecursiveAnalysisSnapshot& snapshot) {
        const auto addresses = snapshot.changed_instruction_addresses();
        const auto values = snapshot.changed_instructions();
        if (addresses.size() != values.size())
            throw std::logic_error(
                "Recursive-Delta enthaelt inkonsistente Instruktionsshards.");
        for (std::size_t index = 0u; index < addresses.size(); ++index) {
            if (addresses[index] != values[index].address)
                throw std::logic_error(
                    "Recursive-Delta bindet Instruktion an falsche Adresse.");
            lines.insert_or_assign(addresses[index], values[index]);
        }
        const auto function_entries = snapshot.changed_function_entries();
        const auto function_values = snapshot.changed_functions();
        if (function_entries.size() != function_values.size())
            throw std::logic_error(
                "Recursive-Delta enthaelt inkonsistente Funktionsshards.");
        for (std::size_t index = 0u; index < function_entries.size(); ++index) {
            if (function_entries[index] != function_values[index].address)
                throw std::logic_error(
                    "Recursive-Delta bindet Funktion an falschen Einstieg.");
            functions.insert_or_assign(function_entries[index],
                                       function_values[index]);
        }
        proven_instruction_addresses.insert(
            snapshot.newly_proven_instruction_addresses().begin(),
            snapshot.newly_proven_instruction_addresses().end());
    }

    [[nodiscard]] std::vector<katana::sh4::DisassemblyLine>
    materialize_lines() const {
        std::vector<katana::sh4::DisassemblyLine> result;
        result.reserve(lines.size());
        for (const auto& [address, line] : lines) {
            static_cast<void>(address);
            result.push_back(line);
        }
        return result;
    }

    [[nodiscard]] const katana::sh4::DisassemblyLine* find(
        const std::uint32_t address) const noexcept {
        const auto found = lines.find(address);
        return found == lines.end() ? nullptr : &found->second;
    }
};

void classify_dynamic_sites(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::io::ExecutableImage& image,
    std::vector<IndirectControlFlowResolution>& resolutions,
    std::span<const ResolvedControlFlowEdge> resolved_edges);

// CFA consumes Recursive's monotone journal through stable function-owner
// shards (and bounded orphan pages). A warm round rebuilds only owners
// touched by changed lines, targets or function boundaries. Dispatch-pattern
// recognition uses its exact widest 48-instruction backward dependency
// window independently, so owner boundaries cannot truncate evidence.
class IncrementalCfaScanCache final {
  public:
    struct DispatchRecognition final {
        std::optional<RelativeCallIslandCandidates> relative_call_island;
        std::optional<JumpTableAnalysis> jump_table;
        std::optional<detail::SnapshotAbsoluteJumpTableProducerEvidence>
            snapshot_absolute_producer;
    };

    void clear() {
        lines_.clear();
        functions_.clear();
        direct_target_references_.clear();
        components_.clear();
        recognition_index_.clear();
        last_changed_dispatches_.clear();
        resolution_index_.clear();
        runtime_copy_index_.clear();
        continuation_index_.clear();
        last_changed_runtime_copies_.clear();
        last_changed_continuations_.clear();
    }

    void apply(const detail::RecursiveAnalysisSnapshot& snapshot,
               const katana::io::ExecutableImage& image,
               JumpTableSnapshotCache& jump_table_cache,
               ControlFlowAnalysisResult& telemetry) {
        last_changed_dispatches_.clear();
        last_changed_runtime_copies_.clear();
        last_changed_continuations_.clear();
        std::set<std::uint32_t> dirty_addresses;
        std::set<std::uint32_t> dirty_component_starts;
        const auto retain_old_component = [&](const std::uint32_t address) {
            const auto found = component_containing(address);
            if (found != components_.end())
                dirty_component_starts.insert(found->first);
        };
        for (const auto& line : snapshot.changed_instructions()) {
            dirty_addresses.insert(line.address);
            retain_old_component(line.address);
            const auto previous = lines_.find(line.address);
            if (previous != lines_.end() &&
                is_indirect_dispatch(
                    previous->second.instruction.kind))
                last_changed_dispatches_.insert(line.address);
            if (previous != lines_.end() &&
                previous->second.target_address.has_value()) {
                retain_old_component(*previous->second.target_address);
                decrement_target(*previous->second.target_address);
                dirty_addresses.insert(*previous->second.target_address);
            }
            lines_.insert_or_assign(line.address, line);
            if (line.target_address.has_value()) {
                ++direct_target_references_[*line.target_address];
                dirty_addresses.insert(*line.target_address);
            }
        }
        const auto changed_function_entries =
            snapshot.changed_function_entries();
        const auto changed_functions = snapshot.changed_functions();
        if (changed_function_entries.size() != changed_functions.size())
            throw std::logic_error(
                "Recursive-Funktionsdelta besitzt inkonsistente Shards.");
        for (std::size_t index = 0u;
             index < changed_function_entries.size(); ++index) {
            const auto entry = changed_function_entries[index];
            const auto previous = functions_.find(entry);
            const bool previous_active = previous != functions_.end();
            const auto previous_size =
                previous_active ? previous->second.size : 0u;
            const bool next_active =
                changed_functions[index].evidence !=
                ControlFlowEvidence::Unresolved;
            const auto next_size =
                next_active ? changed_functions[index].size : 0u;
            const bool shard_contract_changed =
                previous_active != next_active ||
                (previous_active && next_active &&
                 previous_size != next_size);
            if (shard_contract_changed) {
                retain_old_component(entry);
                if (entry >= 2u) retain_old_component(entry - 2u);
            }
            if (previous_active && previous_size != 0u &&
                shard_contract_changed) {
                const auto old_end =
                    static_cast<std::uint64_t>(entry) +
                    previous_size;
                if (old_end <=
                    std::numeric_limits<std::uint32_t>::max())
                    dirty_addresses.insert(
                        static_cast<std::uint32_t>(old_end));
            }
            if (!next_active)
                functions_.erase(entry);
            else
                functions_.insert_or_assign(entry,
                                            changed_functions[index]);
            if (!shard_contract_changed) continue;
            dirty_addresses.insert(entry);
            if (entry >= 2u) dirty_addresses.insert(entry - 2u);
            if (next_active && next_size != 0u) {
                const auto new_end =
                    static_cast<std::uint64_t>(entry) +
                    next_size;
                if (new_end <=
                    std::numeric_limits<std::uint32_t>::max())
                    dirty_addresses.insert(
                        static_cast<std::uint32_t>(new_end));
            }
        }
        if (dirty_addresses.empty()) return;

        for (const auto address : dirty_addresses) {
            if (lines_.contains(address))
                dirty_component_starts.insert(shard_start(address));
        }
        mark_affected_dispatches(
            dirty_addresses, telemetry.jump_table_instruction_visits);

        const auto old_component_starts = dirty_component_starts;
        for (const auto begin : old_component_starts)
            erase_component(begin, telemetry);
        for (const auto address : dirty_addresses) {
            if (lines_.contains(address))
                dirty_component_starts.insert(shard_start(address));
        }

        std::set<std::uint32_t> rebuilt_component_starts;
        for (const auto requested_begin : dirty_component_starts) {
            const auto [begin, end] = shard_bounds(requested_begin);
            if (components_.contains(begin)) continue;
            Component component;
            component.begin = begin;
            component.end = end;
            auto line = lines_.lower_bound(begin);
            while (line != lines_.end() && line->first <= end) {
                component.lines.push_back(line->second);
                ++line;
            }
            telemetry.runtime_copy_instruction_visits +=
                component.lines.size();
            component.runtime_copies =
                analyze_runtime_code_copies(image, component.lines);
            for (const auto& copy : component.runtime_copies.copies) {
                const auto key = std::pair{copy.setup_address,
                                           copy.loop_address};
                runtime_copy_index_.insert_or_assign(key, copy);
                last_changed_runtime_copies_.insert(key);
                ++telemetry.runtime_copy_result_entries_rebuilt;
            }

            std::size_t shard_begin = 0u;
            for (std::size_t index = 1u; index <= component.lines.size();
                 ++index) {
                const bool boundary =
                    index == component.lines.size() ||
                    direct_target_references_.contains(
                        component.lines[index].address);
                if (!boundary) continue;
                const auto shard = std::span<const katana::sh4::DisassemblyLine>{
                    component.lines}.subspan(shard_begin,
                                              index - shard_begin);
                telemetry.local_control_flow_instruction_visits +=
                    shard.size();
                auto local = analyze_local_control_flow(shard, image);
                telemetry.dispatch_index_entries_visited +=
                    local.indirect_control_flow.size() +
                    local.static_return_continuations.size();
                component.local.indirect_control_flow.insert(
                    component.local.indirect_control_flow.end(),
                    std::make_move_iterator(
                        local.indirect_control_flow.begin()),
                    std::make_move_iterator(
                        local.indirect_control_flow.end()));
                component.local.static_return_continuations.insert(
                    component.local.static_return_continuations.end(),
                    std::make_move_iterator(
                        local.static_return_continuations.begin()),
                    std::make_move_iterator(
                        local.static_return_continuations.end()));
                shard_begin = index;
            }
            for (const auto& resolution :
                 component.local.indirect_control_flow) {
                last_changed_dispatches_.insert(
                    resolution.instruction_address);
            }
            for (const auto& continuation :
                 component.local.static_return_continuations) {
                const auto key = std::pair{
                    continuation.instruction_address,
                    continuation.target_address};
                continuation_index_.insert_or_assign(key, continuation);
                last_changed_continuations_.insert(key);
                ++telemetry.local_control_flow_result_entries_rebuilt;
            }
            components_.insert_or_assign(begin, std::move(component));
            rebuilt_component_starts.insert(begin);
        }

        refresh_runtime_copy_dependencies(image, telemetry);

        const auto dirty_dispatches = last_changed_dispatches_;
        for (const auto address : dirty_dispatches) {
            recognition_index_.erase(address);
            const auto dispatch = lines_.find(address);
            if (dispatch == lines_.end() ||
                !is_indirect_dispatch(
                    dispatch->second.instruction.kind))
                continue;
            std::vector<katana::sh4::DisassemblyLine> window;
            window.reserve(maximum_recognition_lookback + 1u);
            auto first = dispatch;
            for (std::size_t distance = 0u;
                 distance < maximum_recognition_lookback &&
                 first != lines_.begin(); ++distance) {
                const auto previous = std::prev(first);
                if (!contiguous(previous->second, first->second)) break;
                first = previous;
            }
            for (auto current = first;; ++current) {
                window.push_back(current->second);
                if (current == dispatch) break;
            }
            const auto dispatch_index = window.size() - 1u;
            if (const auto delay = std::next(dispatch);
                delay != lines_.end() &&
                contiguous(dispatch->second, delay->second))
                window.push_back(delay->second);
            telemetry.jump_table_instruction_visits += window.size();
            DispatchRecognition recognition;
            const auto kind = dispatch->second.instruction.kind;
            if (kind == katana::sh4::InstructionKind::Bsrf) {
                recognition.relative_call_island =
                    recognize_snapshot_relative_call_island_candidates(
                        image, window, dispatch_index);
            } else if (kind == katana::sh4::InstructionKind::Braf) {
                recognition.jump_table =
                    recognize_bounded_relative_jump_table(
                        image, window, dispatch_index,
                        &jump_table_cache);
            } else {
                detail::SnapshotAbsoluteJumpTableProducerEvidence producer;
                recognition.jump_table =
                    detail::recognize_snapshot_absolute_jump_table_candidates_with_producer(
                        image, window, dispatch_index, &producer);
                if (recognition.jump_table.has_value())
                    recognition.snapshot_absolute_producer =
                        std::move(producer);
            }
            if (recognition.relative_call_island.has_value() ||
                recognition.jump_table.has_value())
                recognition_index_.insert_or_assign(
                    address, std::move(recognition));
        }

        // Dynamic-site classification is deliberately delayed until the
        // recognition index for this incremental round is current. A fully
        // resolved immutable BRAF table is intra-function CFG evidence: its
        // case blocks execute with the register state established before the
        // dispatch. Omitting those successors made the bounded writer slice
        // treat every case as a fresh root and lose otherwise exact
        // callee-saved PC-literal targets. Candidate-only or incomplete
        // tables remain excluded, so missing disassembly never manufactures
        // a predecessor.
        for (const auto begin : rebuilt_component_starts) {
            auto component_entry = components_.find(begin);
            if (component_entry == components_.end()) continue;
            auto& component = component_entry->second;
            std::vector<ResolvedControlFlowEdge> recognized_edges;
            auto recognition = recognition_index_.lower_bound(
                component.begin);
            while (recognition != recognition_index_.end() &&
                   recognition->first <= component.end) {
                if (recognition->second.jump_table.has_value()) {
                    const auto& table =
                        *recognition->second.jump_table;
                    const auto evidence =
                        table.evidence == ControlFlowEvidence::Unresolved
                            ? ControlFlowEvidence::ProvenComplete
                            : table.evidence;
                    if (table.dispatch_kind ==
                            JumpTableDispatchKind::Jump &&
                        table.resolved && !table.aot_candidates_only &&
                        control_flow_evidence_complete(evidence)) {
                        for (const auto& entry : table.entries) {
                            if (!entry.accepted) continue;
                            recognized_edges.push_back(
                                ResolvedControlFlowEdge{
                                    table.dispatch_address,
                                    entry.target,
                                    ResolvedControlFlowKind::Jump,
                                    false,
                                    evidence});
                        }
                    }
                }
                ++recognition;
            }
            telemetry.local_control_flow_instruction_visits +=
                component.lines.size();
            classify_dynamic_sites(
                component.lines,
                image,
                component.local.indirect_control_flow,
                recognized_edges);
            for (const auto& resolution :
                 component.local.indirect_control_flow) {
                resolution_index_.insert_or_assign(
                    resolution.instruction_address, resolution);
                ++telemetry.dispatch_index_entries_rebuilt;
            }
        }
    }

    [[nodiscard]] const DispatchRecognition* dispatch_recognition(
        const std::uint32_t address) const noexcept {
        const auto found = recognition_index_.find(address);
        return found == recognition_index_.end() ? nullptr
                                                 : &found->second;
    }

    [[nodiscard]] const std::set<std::uint32_t>&
    changed_dispatches() const noexcept {
        return last_changed_dispatches_;
    }

    [[nodiscard]] const auto& changed_runtime_copies() const noexcept {
        return last_changed_runtime_copies_;
    }

    [[nodiscard]] const auto& changed_continuations() const noexcept {
        return last_changed_continuations_;
    }

    [[nodiscard]] const RuntimeCodeCopy* runtime_copy(
        const std::pair<std::uint32_t, std::uint32_t> key) const noexcept {
        const auto found = runtime_copy_index_.find(key);
        return found == runtime_copy_index_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const StaticReturnContinuationCandidate* continuation(
        const std::pair<std::uint32_t, std::uint32_t> key) const noexcept {
        const auto found = continuation_index_.find(key);
        return found == continuation_index_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const IndirectControlFlowResolution* base_resolution(
        const std::uint32_t address) const noexcept {
        const auto found = resolution_index_.find(address);
        return found == resolution_index_.end() ? nullptr : &found->second;
    }

  private:
    struct Component final {
        std::uint32_t begin = 0u;
        std::uint32_t end = 0u;
        std::vector<katana::sh4::DisassemblyLine> lines;
        RuntimeCodeCopyAnalysis runtime_copies;
        LocalControlFlowAnalysis local;
    };

    static constexpr std::size_t maximum_recognition_lookback = 48u;
    static constexpr std::uint32_t maximum_recognition_window_bytes =
        static_cast<std::uint32_t>(maximum_recognition_lookback * 2u);
    static constexpr std::uint32_t orphan_shard_bytes = 4096u;

    [[nodiscard]] static bool contiguous(
        const katana::sh4::DisassemblyLine& left,
        const katana::sh4::DisassemblyLine& right) noexcept {
        return left.address <=
                   std::numeric_limits<std::uint32_t>::max() - 2u &&
               left.address + 2u == right.address;
    }

    [[nodiscard]] static bool is_indirect_dispatch(
        const katana::sh4::InstructionKind kind) noexcept {
        return kind == katana::sh4::InstructionKind::Jmp ||
               kind == katana::sh4::InstructionKind::Jsr ||
               kind == katana::sh4::InstructionKind::Braf ||
               kind == katana::sh4::InstructionKind::Bsrf;
    }

    [[nodiscard]] auto component_containing(
        const std::uint32_t address) {
        auto component = components_.upper_bound(address);
        if (component == components_.begin()) return components_.end();
        --component;
        return address <= component->second.end ? component
                                                : components_.end();
    }

    [[nodiscard]] std::uint32_t shard_start(
        const std::uint32_t address) const {
        const auto next = functions_.upper_bound(address);
        if (next != functions_.begin()) {
            const auto owner = std::prev(next);
            const auto size = owner->second.size;
            const auto inside =
                size == 0u ||
                static_cast<std::uint64_t>(address) <
                    static_cast<std::uint64_t>(owner->first) + size;
            if (inside) return owner->first;
        }
        auto page = address & ~(orphan_shard_bytes - 1u);
        if (next != functions_.begin()) {
            const auto previous = std::prev(next);
            if (previous->second.size != 0u) {
                const auto after =
                    static_cast<std::uint64_t>(previous->first) +
                    previous->second.size;
                if (after <= std::numeric_limits<std::uint32_t>::max())
                    page = std::max(
                        page, static_cast<std::uint32_t>(after));
            }
        }
        return page;
    }

    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
    shard_bounds(const std::uint32_t requested_start) const {
        const auto function = functions_.find(requested_start);
        if (function != functions_.end()) {
            std::uint64_t end =
                std::numeric_limits<std::uint32_t>::max();
            if (function->second.size != 0u)
                end = static_cast<std::uint64_t>(requested_start) +
                      function->second.size - 1u;
            const auto next = std::next(function);
            if (next != functions_.end() && next->first != 0u)
                end = std::min<std::uint64_t>(end,
                                              next->first - 1u);
            return {requested_start,
                    static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                            end,
                            std::numeric_limits<std::uint32_t>::max()))};
        }
        auto end = static_cast<std::uint64_t>(requested_start) +
                   orphan_shard_bytes - 1u;
        const auto next = functions_.upper_bound(requested_start);
        if (next != functions_.end() && next->first != 0u)
            end = std::min<std::uint64_t>(end, next->first - 1u);
        return {requested_start,
                static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    end,
                    std::numeric_limits<std::uint32_t>::max()))};
    }

    void mark_affected_dispatches(
        const std::set<std::uint32_t>& dirty_addresses,
        std::size_t& instruction_visits) {
        // Every old dirty-address walk only observes the first contiguous
        // 48-instruction suffix starting at the greatest decoded address not
        // greater than the dirty address.  Several dirty bytes normally map
        // to overlapping suffixes, so visiting each suffix independently
        // makes the same decoded instructions dominate an otherwise
        // incremental CFA round.  Coalesce only the physical visit: an
        // indirect dispatch is still marked iff at least one of the original
        // contiguous 48-instruction windows contains it.
        std::vector<std::uint32_t> window_starts;
        window_starts.reserve(dirty_addresses.size());
        for (const auto address : dirty_addresses) {
            auto current = lines_.lower_bound(address);
            if (current == lines_.end()) continue;
            if (current->first != address) {
                if (current == lines_.begin()) continue;
                --current;
            }
            window_starts.push_back(current->first);
        }
        if (window_starts.empty()) return;
        std::sort(window_starts.begin(), window_starts.end());
        window_starts.erase(
            std::unique(window_starts.begin(), window_starts.end()),
            window_starts.end());

        const auto window_end = [](const std::uint32_t begin) noexcept {
            return begin > std::numeric_limits<std::uint32_t>::max() -
                               maximum_recognition_window_bytes
                       ? std::numeric_limits<std::uint32_t>::max()
                       : begin + maximum_recognition_window_bytes;
        };

        std::size_t group_begin = 0u;
        while (group_begin < window_starts.size()) {
            auto group_end = window_end(window_starts[group_begin]);
            auto group_limit = group_begin + 1u;
            // Merge only numerically overlapping windows.  Contiguity is
            // checked again below, so a decoded-image gap cannot make a
            // dispatch visible to a window that previously stopped there.
            while (group_limit < window_starts.size() &&
                   window_starts[group_limit] <= group_end) {
                group_end = std::max(
                    group_end, window_end(window_starts[group_limit]));
                ++group_limit;
            }

            auto current = lines_.lower_bound(window_starts[group_begin]);
            const katana::sh4::DisassemblyLine* previous = nullptr;
            std::optional<std::uint32_t> active_window_end;
            auto next_window = group_begin;
            while (current != lines_.end() && current->first <= group_end) {
                ++instruction_visits;
                if (previous != nullptr &&
                    !contiguous(*previous, current->second))
                    active_window_end.reset();
                while (next_window < group_limit &&
                       window_starts[next_window] <= current->first) {
                    // Every start came from lines_, hence an earlier start
                    // can only be reached through a contiguous predecessor
                    // or across a gap that has just reset the active window.
                    const auto end = window_end(window_starts[next_window]);
                    if (!active_window_end.has_value() ||
                        *active_window_end < end)
                        active_window_end = end;
                    ++next_window;
                }
                if (active_window_end.has_value() &&
                    current->first <= *active_window_end &&
                    is_indirect_dispatch(current->second.instruction.kind))
                    last_changed_dispatches_.insert(current->first);
                previous = &current->second;
                ++current;
            }
            group_begin = group_limit;
        }
    }

    void erase_component(const std::uint32_t begin,
                         ControlFlowAnalysisResult& telemetry) {
        const auto component = components_.find(begin);
        if (component == components_.end()) return;
        for (const auto& resolution :
             component->second.local.indirect_control_flow) {
            last_changed_dispatches_.insert(
                resolution.instruction_address);
            if (resolution_index_.erase(
                    resolution.instruction_address) != 0u)
                ++telemetry.dispatch_index_entries_rebuilt;
        }
        for (const auto& copy :
             component->second.runtime_copies.copies) {
            const auto key = std::pair{copy.setup_address,
                                       copy.loop_address};
            if (runtime_copy_index_.erase(key) != 0u)
                ++telemetry.runtime_copy_result_entries_rebuilt;
            last_changed_runtime_copies_.insert(key);
        }
        for (const auto& continuation :
             component->second.local.static_return_continuations) {
            const auto key = std::pair{
                continuation.instruction_address,
                continuation.target_address};
            if (continuation_index_.erase(key) != 0u)
                ++telemetry.local_control_flow_result_entries_rebuilt;
            last_changed_continuations_.insert(key);
        }
        components_.erase(component);
    }

    void decrement_target(const std::uint32_t address) {
        const auto found = direct_target_references_.find(address);
        if (found == direct_target_references_.end()) return;
        if (--found->second == 0u) direct_target_references_.erase(found);
    }

    [[nodiscard]] static bool same_runtime_code_patch(
        const RuntimeCodePatchCandidate& left,
        const RuntimeCodePatchCandidate& right) noexcept {
        return left.store_instruction_address ==
                   right.store_instruction_address &&
               left.slot_address == right.slot_address &&
               left.live_value == right.live_value &&
               left.target_address == right.target_address;
    }

    [[nodiscard]] static bool same_runtime_code_mutable_range(
        const RuntimeCodeMutableRangeCandidate& left,
        const RuntimeCodeMutableRangeCandidate& right) noexcept {
        return left.store_instruction_address ==
                   right.store_instruction_address &&
               left.load_instruction_address ==
                   right.load_instruction_address &&
               left.slot_address == right.slot_address &&
               left.size == right.size;
    }

    [[nodiscard]] static bool same_runtime_code_copy(
        const RuntimeCodeCopy& left,
        const RuntimeCodeCopy& right) noexcept {
        return left.setup_address == right.setup_address &&
               left.loop_address == right.loop_address &&
               left.source_begin == right.source_begin &&
               left.source_end_inclusive ==
                   right.source_end_inclusive &&
               left.source_byte_count == right.source_byte_count &&
               left.destination_vbr_delta ==
                   right.destination_vbr_delta &&
               left.mutable_range_analysis_complete ==
                   right.mutable_range_analysis_complete &&
               left.evidence == right.evidence &&
               left.aot_candidates_only == right.aot_candidates_only &&
               left.reason == right.reason &&
               left.patch_candidates.size() ==
                   right.patch_candidates.size() &&
               std::equal(left.patch_candidates.begin(),
                          left.patch_candidates.end(),
                          right.patch_candidates.begin(),
                          same_runtime_code_patch) &&
               left.mutable_ranges.size() == right.mutable_ranges.size() &&
               std::equal(left.mutable_ranges.begin(),
                          left.mutable_ranges.end(),
                          right.mutable_ranges.begin(),
                          same_runtime_code_mutable_range);
    }

    void refresh_runtime_copy_dependencies(
        const katana::io::ExecutableImage& image,
        ControlFlowAnalysisResult& telemetry) {
        for (auto& [key, copy] : runtime_copy_index_) {
            const auto owner = component_containing(copy.setup_address);
            if (owner == components_.end())
                throw std::logic_error(
                    "Runtime-Codecopy besitzt keinen inkrementellen Owner.");

            // Recognition belongs to the copy-loop owner, but its exact AOT
            // contract also depends on pre-copy patch stores, the copied
            // template body, and every direct entry into that body.  Those
            // dependencies may live in other function-owner shards.  Rebuild
            // the small union here so componentization cannot silently drop
            // patch targets or proven mutable scratch slots.
            std::vector<katana::sh4::DisassemblyLine> dependency_lines =
                owner->second.lines;
            const auto source_end =
                static_cast<std::uint64_t>(copy.source_begin) +
                copy.source_byte_count;
            auto source = lines_.lower_bound(copy.source_begin);
            while (source != lines_.end() &&
                   source->first < source_end) {
                dependency_lines.push_back(source->second);
                ++source;
            }
            for (const auto& [address, line] : lines_) {
                static_cast<void>(address);
                if (!line.target_address.has_value()) continue;
                const auto target = *line.target_address;
                if (target > copy.source_begin && target < source_end)
                    dependency_lines.push_back(line);
            }
            std::sort(dependency_lines.begin(),
                      dependency_lines.end(),
                      [](const auto& left, const auto& right) {
                          return left.address < right.address;
                      });
            dependency_lines.erase(
                std::unique(dependency_lines.begin(),
                            dependency_lines.end(),
                            [](const auto& left, const auto& right) {
                                return left.address == right.address;
                            }),
                dependency_lines.end());
            telemetry.runtime_copy_instruction_visits +=
                dependency_lines.size();

            const auto completed = analyze_runtime_code_copies(
                image, dependency_lines);
            const auto rebuilt = std::find_if(
                completed.copies.begin(),
                completed.copies.end(),
                [&](const auto& candidate) {
                    return candidate.setup_address == key.first &&
                           candidate.loop_address == key.second;
                });
            if (rebuilt == completed.copies.end())
                throw std::logic_error(
                    "Runtime-Codecopy konnte aus ihrem vollstaendigen "
                    "Abhaengigkeitsfenster nicht rekonstruiert werden.");
            if (same_runtime_code_copy(copy, *rebuilt)) continue;
            copy = *rebuilt;
            last_changed_runtime_copies_.insert(key);
            ++telemetry.runtime_copy_result_entries_rebuilt;
        }
    }

    std::map<std::uint32_t, katana::sh4::DisassemblyLine> lines_;
    std::map<std::uint32_t, FunctionCandidate> functions_;
    std::map<std::uint32_t, std::size_t> direct_target_references_;
    std::map<std::uint32_t, Component> components_;
    std::map<std::uint32_t, DispatchRecognition> recognition_index_;
    std::set<std::uint32_t> last_changed_dispatches_;
    std::map<std::uint32_t, IndirectControlFlowResolution>
        resolution_index_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, RuntimeCodeCopy>
        runtime_copy_index_;
    std::map<std::pair<std::uint32_t, std::uint32_t>,
             StaticReturnContinuationCandidate>
        continuation_index_;
    std::set<std::pair<std::uint32_t, std::uint32_t>>
        last_changed_runtime_copies_;
    std::set<std::pair<std::uint32_t, std::uint32_t>>
        last_changed_continuations_;
};

const katana::sh4::DisassemblyLine* find_instruction(const RecursiveAnalysisResult& result,
                                                     const std::uint32_t address) {
    const auto iterator = std::lower_bound(
        result.instructions.begin(),
        result.instructions.end(),
        address,
        [](const auto& line, const std::uint32_t candidate) { return line.address < candidate; });
    return iterator != result.instructions.end() && iterator->address == address ? &*iterator
                                                                                 : nullptr;
}

void mark_resolved_table_dispatch(std::vector<IndirectControlFlowResolution>& resolutions,
                                  const JumpTableAnalysis& table) {
    const auto resolution =
        std::find_if(resolutions.begin(), resolutions.end(), [&table](const auto& candidate) {
            return candidate.instruction_address == table.dispatch_address;
        });
    if (resolution == resolutions.end()) return;
    resolution->origin_class = IndirectControlFlowOriginClass::Table;
    resolution->evidence_origins = {table.evidence == ControlFlowEvidence::HintCandidate
                                        ? AnalysisEvidenceOrigin::UserHint
                                    : table.evidence == ControlFlowEvidence::ForcedOverride
                                        ? AnalysisEvidenceOrigin::UserOverride
                                        : AnalysisEvidenceOrigin::JumpTable};
    if (table.aot_candidates_only) {
        resolution->status = ResolutionStatus::Unresolved;
        resolution->evidence = ControlFlowEvidence::RuntimeOnly;
        resolution->target.reset();
        resolution->targets.clear();
        resolution->analysis_candidates.clear();
        for (const auto& entry : table.entries)
            if (entry.accepted) resolution->analysis_candidates.push_back(entry.target);
        std::sort(resolution->analysis_candidates.begin(),
                  resolution->analysis_candidates.end());
        resolution->analysis_candidates.erase(
            std::unique(resolution->analysis_candidates.begin(),
                        resolution->analysis_candidates.end()),
            resolution->analysis_candidates.end());
        resolution->reason = "runtime-contract-" + table.reason;
        return;
    }
    if (!table.resolved) {
        if (table.reason == "table-segment-writable") {
            resolution->status = ResolutionStatus::Unresolved;
            resolution->evidence = ControlFlowEvidence::RuntimeOnly;
            resolution->target.reset();
            resolution->targets.clear();
            resolution->reason = "dynamic-writable-table";
        } else {
            resolution->reason = table.reason;
        }
        return;
    }
    resolution->status = ResolutionStatus::Resolved;
    resolution->evidence = table.evidence;
    if (table.evidence == ControlFlowEvidence::HintCandidate)
        resolution->status = ResolutionStatus::Unresolved;
    else if (!control_flow_evidence_complete(table.evidence))
        resolution->status = ResolutionStatus::Guarded;
    resolution->target.reset();
    resolution->targets.clear();
    for (const auto& entry : table.entries)
        resolution->targets.push_back(entry.target);
    std::sort(resolution->targets.begin(), resolution->targets.end());
    resolution->targets.erase(std::unique(resolution->targets.begin(), resolution->targets.end()),
                              resolution->targets.end());
    resolution->reason = table.reason;
}

void mark_resolved_table_dispatch(
    std::map<std::uint32_t, IndirectControlFlowResolution>& resolutions,
    const JumpTableAnalysis& table) {
    const auto resolution = resolutions.find(table.dispatch_address);
    if (resolution == resolutions.end()) return;
    auto& result = resolution->second;
    result.origin_class = IndirectControlFlowOriginClass::Table;
    result.evidence_origins = {
        table.evidence == ControlFlowEvidence::HintCandidate
            ? AnalysisEvidenceOrigin::UserHint
        : table.evidence == ControlFlowEvidence::ForcedOverride
            ? AnalysisEvidenceOrigin::UserOverride
            : AnalysisEvidenceOrigin::JumpTable};
    if (table.aot_candidates_only) {
        result.status = ResolutionStatus::Unresolved;
        result.evidence = ControlFlowEvidence::RuntimeOnly;
        result.target.reset();
        result.targets.clear();
        result.analysis_candidates.clear();
        for (const auto& entry : table.entries) {
            if (entry.accepted)
                result.analysis_candidates.push_back(entry.target);
        }
        std::sort(result.analysis_candidates.begin(),
                  result.analysis_candidates.end());
        result.analysis_candidates.erase(
            std::unique(result.analysis_candidates.begin(),
                        result.analysis_candidates.end()),
            result.analysis_candidates.end());
        result.reason = "runtime-contract-" + table.reason;
        return;
    }
    if (!table.resolved) {
        if (table.reason == "table-segment-writable") {
            result.status = ResolutionStatus::Unresolved;
            result.evidence = ControlFlowEvidence::RuntimeOnly;
            result.target.reset();
            result.targets.clear();
            result.reason = "dynamic-writable-table";
        } else {
            result.reason = table.reason;
        }
        return;
    }
    result.status = ResolutionStatus::Resolved;
    result.evidence = table.evidence;
    if (table.evidence == ControlFlowEvidence::HintCandidate)
        result.status = ResolutionStatus::Unresolved;
    else if (!control_flow_evidence_complete(table.evidence))
        result.status = ResolutionStatus::Guarded;
    result.target.reset();
    result.targets.clear();
    for (const auto& entry : table.entries) {
        if (entry.accepted)
            result.targets.push_back(entry.target);
    }
    std::sort(result.targets.begin(), result.targets.end());
    result.targets.erase(
        std::unique(result.targets.begin(), result.targets.end()),
        result.targets.end());
    result.reason = table.reason;
}

bool memory_load(const katana::sh4::InstructionKind kind) {
    using K = katana::sh4::InstructionKind;
    switch (kind) {
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
    case K::MovWordLoadPcRelative:
    case K::MovLongLoadPcRelative:
        return true;
    default:
        return false;
    }
}

struct BackwardSlice {
    std::set<std::uint32_t> writers;
    // Path completeness and writer shape are separate contracts.  A unique
    // register-copy writer is a useful first link in a static value chain even
    // though it is not, by itself, the memory-writer shape used by the dynamic
    // origin classifier.
    bool path_incomplete = false;
    bool budget_exhausted = false;
    bool writer_not_memory_load = false;
    bool preceding_call = false;
};

struct WriterSliceLocation {
    std::uint32_t block = 0u;
    std::size_t before_index = 0u;
};

struct WriterSliceBlockEvents {
    std::array<std::vector<std::size_t>, 16u> register_writes;
    std::vector<std::size_t> calls;
};

struct WriterSliceIndex {
    std::unordered_map<std::uint32_t, const BasicBlock*> by_start;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> predecessors;
    std::unordered_map<std::uint32_t, WriterSliceLocation> locations;
    std::unordered_map<std::uint32_t, WriterSliceBlockEvents> events;
};

WriterSliceIndex build_writer_slice_index(const std::span<const BasicBlock> blocks) {
    WriterSliceIndex index;
    index.by_start.reserve(blocks.size());
    index.predecessors.reserve(blocks.size());
    index.events.reserve(blocks.size());
    std::size_t instruction_count = 0u;
    for (const auto& block : blocks)
        instruction_count += block.lines.size();
    index.locations.reserve(instruction_count);
    for (const auto& block : blocks) {
        index.by_start.emplace(block.start_address, &block);
        auto& events = index.events[block.start_address];
        for (const auto successor : block.successors)
            index.predecessors[successor].push_back(block.start_address);
        for (std::size_t line_index = 0u; line_index < block.lines.size(); ++line_index) {
            const auto& line = block.lines[line_index];
            index.locations.insert_or_assign(
                line.address,
                WriterSliceLocation{block.start_address, line_index});
            if (line.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::IndirectCall)
                events.calls.push_back(line_index);
            const auto write_mask = general_register_write_mask(line.instruction);
            for (std::uint8_t register_index = 0u;
                 register_index < events.register_writes.size();
                 ++register_index) {
                if ((write_mask & static_cast<std::uint16_t>(
                                      1u << register_index)) != 0u)
                    events.register_writes[register_index].push_back(line_index);
            }
        }
    }
    return index;
}

BackwardSlice bounded_writer_slice(const WriterSliceIndex& index,
                                   const std::uint32_t before_address,
                                   const std::uint8_t register_index,
                                   const std::size_t work_state_budget = 128u) {
    BackwardSlice result;
    if (register_index >= 16u) {
        result.path_incomplete = true;
        return result;
    }
    const auto initial = index.locations.find(before_address);
    if (initial == index.locations.end()) {
        result.path_incomplete = true;
        return result;
    }
    struct Work {
        std::uint32_t block = 0u;
        std::size_t before_index = 0u;
    };
    std::deque<Work> pending{
        {initial->second.block, initial->second.before_index}};
    std::set<std::pair<std::uint32_t, std::size_t>> visited;
    while (!pending.empty()) {
        const auto work = pending.front();
        pending.pop_front();
        if (!visited.emplace(work.block, work.before_index).second) continue;
        if (visited.size() > work_state_budget) {
            result.path_incomplete = true;
            result.budget_exhausted = true;
            break;
        }
        const auto found = index.by_start.find(work.block);
        if (found == index.by_start.end()) {
            result.path_incomplete = true;
            continue;
        }
        const auto& block = *found->second;
        bool writer_found = false;
        const auto block_events = index.events.find(block.start_address);
        if (block_events == index.events.end()) {
            result.path_incomplete = true;
            continue;
        }
        const auto& writes =
            block_events->second.register_writes[register_index];
        const auto writer_after = std::lower_bound(
            writes.begin(), writes.end(), work.before_index);
        const auto& calls = block_events->second.calls;
        const auto call_after = std::lower_bound(
            calls.begin(), calls.end(), work.before_index);
        if (writer_after != writes.begin()) {
            const auto line_index = *std::prev(writer_after);
            if (call_after != calls.begin() &&
                *std::prev(call_after) > line_index)
                result.preceding_call = true;
            const auto& line = block.lines[line_index];
            writer_found = true;
            result.writers.insert(line.address);
            if (line.instruction.destination_register != register_index ||
                !memory_load(line.instruction.kind))
                result.writer_not_memory_load = true;
        } else if (call_after != calls.begin()) {
            result.preceding_call = true;
        }
        if (writer_found) continue;
        const auto incoming = index.predecessors.find(block.start_address);
        if (incoming == index.predecessors.end() || incoming->second.empty()) {
            result.path_incomplete = true;
            continue;
        }
        for (const auto predecessor : incoming->second) {
            const auto predecessor_block = index.by_start.find(predecessor);
            if (predecessor_block != index.by_start.end())
                pending.push_back(
                    {predecessor, predecessor_block->second->lines.size()});
        }
    }
    return result;
}

const katana::sh4::DisassemblyLine* find_line(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::uint32_t address) {
    const auto found = std::lower_bound(
        lines.begin(), lines.end(), address,
        [](const auto& line, const auto value) { return line.address < value; });
    return found != lines.end() && found->address == address ? &*found : nullptr;
}

struct StaticCodePointerChain {
    std::optional<std::uint32_t> target;
    std::set<std::uint32_t> definition_sites;
    bool complete = false;
    bool preceding_call = false;
    bool preceding_call_transport_abi_proven = true;
    bool exact_target_guard = false;
    ExactGuardRejectionReason exact_target_rejection_reason =
        ExactGuardRejectionReason::None;
};

struct StaticCodePointerSet {
    std::vector<std::uint32_t> targets;
    std::set<std::uint32_t> definition_sites;
    bool complete = false;
    ExactGuardRejectionReason exact_target_rejection_reason =
        ExactGuardRejectionReason::None;
};

// Resolve the compiler form in which a closed conditional chooses between
// exactly two PC-relative code literals before a shared JSR/JMP.  This is not
// a value-analysis hint: the backward CFG slice must account for every
// predecessor path, and both first writers, literal cells, target opcodes and
// the dispatch must belong to the same authenticated immutable image view.
// The live register is still checked against the complete two-target set by
// GuardedComplete codegen; an unexpected value aborts before the global
// dispatcher.
StaticCodePointerSet resolve_static_code_pointer_set(
    const WriterSliceIndex& index,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::io::ExecutableImage& image,
    const std::uint32_t dispatch_address,
    const std::uint8_t register_index) {
    StaticCodePointerSet result;
    auto slice = bounded_writer_slice(
        index, dispatch_address, register_index);
    if (slice.budget_exhausted && register_index >= 8u &&
        register_index <= 14u) {
        constexpr std::size_t extended_work_state_budget = 512u;
        slice = bounded_writer_slice(
            index,
            dispatch_address,
            register_index,
            extended_work_state_budget);
    }
    result.definition_sites = slice.writers;
    if (slice.path_incomplete || slice.budget_exhausted ||
        slice.writer_not_memory_load || slice.preceding_call ||
        slice.writers.size() != 2u) {
        if (slice.preceding_call)
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::PrecedingCall;
        return result;
    }

    const auto immutable_image_proof =
        [&](const std::uint32_t address,
            const bool require_executable,
            const std::size_t width)
        -> const katana::io::ImageImmutableRange* {
        const auto* segment = image.find_segment(address, width);
        if (segment == nullptr || !segment->permissions.readable ||
            segment->source_kind ==
                katana::io::ImageSourceKind::RuntimeMemory ||
            (require_executable && !segment->permissions.executable))
            return nullptr;
        return image.find_immutable_range(address, width);
    };
    const auto same_authenticated_view =
        [](const katana::io::ImageImmutableRange* left,
           const katana::io::ImageImmutableRange* right) {
        return left != nullptr && right != nullptr &&
               left->generation == right->generation;
    };
    const auto* const dispatch_proof =
        immutable_image_proof(dispatch_address, true, 2u);
    if (dispatch_proof == nullptr) {
        result.exact_target_rejection_reason =
            ExactGuardRejectionReason::DispatchImmutableProofMissing;
        return result;
    }

    using K = katana::sh4::InstructionKind;
    for (const auto writer_address : slice.writers) {
        const auto* const writer = find_line(lines, writer_address);
        if (writer == nullptr ||
            writer->instruction.destination_register != register_index ||
            writer->instruction.kind != K::MovLongLoadPcRelative) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::ConflictingTargets;
            return result;
        }
        const auto* const writer_proof =
            immutable_image_proof(writer_address, true, 2u);
        if (writer_proof == nullptr) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::WriterImmutableProofMissing;
            return result;
        }
        if (writer_address >
            std::numeric_limits<std::uint32_t>::max() - 4u) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::LiteralImmutableProofMissing;
            return result;
        }
        const auto base =
            static_cast<std::uint64_t>((writer_address + 4u) & ~3u);
        const auto displacement =
            static_cast<std::int64_t>(writer->instruction.displacement);
        if (displacement < 0 ||
            base + static_cast<std::uint64_t>(displacement) >
                std::numeric_limits<std::uint32_t>::max()) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::LiteralImmutableProofMissing;
            return result;
        }
        const auto literal_address = static_cast<std::uint32_t>(
            base + static_cast<std::uint64_t>(displacement));
        const auto resolved_literal =
            image.resolve_segment_address(literal_address, 4u);
        const auto* const literal_proof =
            resolved_literal.has_value()
                ? immutable_image_proof(*resolved_literal, false, 4u)
                : nullptr;
        if (!resolved_literal.has_value() || literal_proof == nullptr) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::LiteralImmutableProofMissing;
            return result;
        }

        const auto candidate = image.read_u32_le(*resolved_literal);
        const auto validation = validate_decode_candidate(image, candidate);
        if (!validation.valid()) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::TargetNotExecutable;
            return result;
        }
        const auto* const target_segment =
            image.find_segment(validation.resolved_address, 2u);
        const auto target_offset =
            target_segment != nullptr
                ? target_segment->byte_offset(validation.resolved_address)
                : std::optional<std::size_t>{};
        const bool target_opcode_known =
            target_segment != nullptr && target_offset.has_value() &&
            *target_offset <= target_segment->bytes.size() &&
            target_segment->bytes.size() - *target_offset >= 2u &&
            katana::sh4::decode(katana::io::read_u16_le(
                target_segment->bytes, *target_offset))
                .is_known();
        if (!target_opcode_known) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::UnknownTargetOpcode;
            return result;
        }
        const auto* const target_proof =
            immutable_image_proof(validation.resolved_address, true, 2u);
        if (target_proof == nullptr) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::TargetImmutableProofMissing;
            return result;
        }
        if (!same_authenticated_view(dispatch_proof, writer_proof) ||
            !same_authenticated_view(dispatch_proof, literal_proof) ||
            !same_authenticated_view(dispatch_proof, target_proof)) {
            result.exact_target_rejection_reason =
                ExactGuardRejectionReason::ImageIdentityMismatch;
            return result;
        }
        result.targets.push_back(validation.resolved_address);
    }
    std::sort(result.targets.begin(), result.targets.end());
    result.targets.erase(
        std::unique(result.targets.begin(), result.targets.end()),
        result.targets.end());
    if (result.targets.size() != 2u) {
        result.targets.clear();
        result.exact_target_rejection_reason =
            ExactGuardRejectionReason::ConflictingTargets;
        return result;
    }
    result.complete = true;
    return result;
}

// Resolve the common compiler form
//
//   mov.l @(literal,pc), rN  [; mov rN,rM] [; add #imm,rM] ; jsr/jmp @rM
//
// without promoting mutable/runtime values to complete control-flow proof.
// Every incoming CFG path must reach the same unique writer chain.  A chain is
// eligible for an exact edge only when its source bytes are identity-bound,
// every intervening call transports the value in a nonvolatile register of
// the image's proven guest ABI, and the final address is an immutable
// executable entry. MOVA is admitted under the same contract for
// compiler-generated local thunk addresses.
StaticCodePointerChain resolve_static_code_pointer_chain(
    const WriterSliceIndex& index,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::io::ExecutableImage& image,
    std::uint32_t before_address,
    std::uint8_t register_index) {
    StaticCodePointerChain result;
    const auto dispatch_address = before_address;
    std::int64_t addend = 0;
    std::set<std::pair<std::uint32_t, std::uint8_t>> visited;
    constexpr std::size_t maximum_chain_depth = 8u;
    for (std::size_t depth = 0u; depth < maximum_chain_depth; ++depth) {
        if (!visited.emplace(before_address, register_index).second) return result;
        auto slice = bounded_writer_slice(
            index, before_address, register_index);
        // The per-block event index skips instructions that cannot change the
        // requested register or its call-transport proof. The remaining hard
        // bound is therefore over distinct CFG work states rather than source
        // distance. Retry a larger state frontier only for SH-C nonvolatile
        // registers; unknown predecessors, conflicting writers and either
        // state bound ending still fail closed.
        if (slice.budget_exhausted && register_index >= 8u &&
            register_index <= 14u) {
            constexpr std::size_t extended_work_state_budget = 512u;
            slice = bounded_writer_slice(
                index,
                before_address,
                register_index,
                extended_work_state_budget);
        }
        result.preceding_call = result.preceding_call || slice.preceding_call;
        if (slice.preceding_call) {
            result.preceding_call_transport_abi_proven =
                result.preceding_call_transport_abi_proven &&
                image.guest_call_abi() ==
                    katana::io::GuestCallAbi::SuperHC &&
                register_index >= 8u && register_index <= 14u;
        }
        result.definition_sites.insert(slice.writers.begin(), slice.writers.end());
        // r8-r14 are the SH-C ABI's nonvolatile general registers. With an
        // unknown ABI, crossing a call in one of them remains guarded AOT
        // inventory only: a non-conforming callee can still change the live
        // target. An explicitly bound SuperHC image proves that transport and
        // may retain the exact immutable chain. Caller-saved chains remain
        // cut at every call.
        const bool call_transport_admissible =
            !slice.preceding_call ||
            (register_index >= 8u && register_index <= 14u);
        if (slice.path_incomplete || !call_transport_admissible ||
            slice.writers.size() != 1u)
            return result;
        const auto* const writer = find_line(lines, *slice.writers.begin());
        if (writer == nullptr ||
            writer->instruction.destination_register != register_index)
            return result;
        using K = katana::sh4::InstructionKind;
        if (writer->instruction.kind == K::MovRegister) {
            register_index = writer->instruction.source_register;
            before_address = writer->address;
            continue;
        }
        if (writer->instruction.kind == K::AddImmediate) {
            addend += writer->instruction.immediate;
            before_address = writer->address;
            continue;
        }

        std::optional<std::uint32_t> candidate;
        if (writer->instruction.kind == K::MoveAddressPcRelative) {
            const auto address =
                static_cast<std::int64_t>((writer->address + 4u) & ~3u) +
                writer->instruction.displacement + addend;
            candidate = static_cast<std::uint32_t>(address);
        } else if (writer->instruction.kind == K::MovLongLoadPcRelative) {
            const auto literal_address =
                ((writer->address + 4u) & ~3u) +
                static_cast<std::uint32_t>(writer->instruction.displacement);
            const auto resolved_literal =
                image.resolve_segment_address(literal_address, 4u);
            const auto* const segment =
                resolved_literal.has_value()
                    ? image.find_segment(*resolved_literal, 4u)
                    : nullptr;
            const auto offset =
                segment != nullptr
                    ? segment->byte_offset(*resolved_literal)
                    : std::optional<std::size_t>{};
            if (segment == nullptr || !segment->permissions.readable ||
                !offset.has_value() || *offset > segment->bytes.size() ||
                4u > segment->bytes.size() - *offset)
                return result;
            candidate = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(image.read_u32_le(*resolved_literal)) + addend);
        } else {
            return result;
        }
        const auto validation = validate_decode_candidate(image, *candidate);
        if (!validation.valid()) return result;
        result.target = validation.resolved_address;
        result.complete = true;
        const auto immutable_image_proof =
            [&](const std::uint32_t address,
                const bool require_executable,
                const std::size_t width)
            -> const katana::io::ImageImmutableRange* {
            const auto* segment = image.find_segment(address, width);
            if (segment == nullptr || !segment->permissions.readable ||
                segment->source_kind ==
                    katana::io::ImageSourceKind::RuntimeMemory ||
                (require_executable && !segment->permissions.executable))
                return nullptr;
            return image.find_immutable_range(address, width);
        };
        const auto* const target_segment = image.find_segment(
            validation.resolved_address, 2u);
        bool target_opcode_known = false;
        if (target_segment != nullptr) {
            const auto target_offset =
                target_segment->byte_offset(validation.resolved_address);
            if (target_offset.has_value() &&
                *target_offset <= target_segment->bytes.size() &&
                target_segment->bytes.size() - *target_offset >= 2u)
                target_opcode_known = katana::sh4::decode(
                    katana::io::read_u16_le(target_segment->bytes, *target_offset))
                                          .is_known();
        }
        // The exact edge is an identity-bound proof over the complete
        // dispatch chain, not merely over its terminal literal load.  Every
        // writer and the dispatch instruction itself must remain executable
        // immutable bytes in this same ExecutableImage; otherwise a mutable
        // intermediate register writer could redirect the chain between the
        // static analysis and the guarded runtime edge.
        const auto* const dispatch_proof =
            immutable_image_proof(dispatch_address, true, 2u);
        const auto* const writer_proof =
            immutable_image_proof(writer->address, true, 2u);
        const auto* const target_proof =
            immutable_image_proof(validation.resolved_address, true, 2u);
        const bool immutable_dispatch = dispatch_proof != nullptr;
        const bool immutable_writer = writer_proof != nullptr;
        const bool immutable_target = target_proof != nullptr;
        bool immutable_literal = immutable_writer;
        const katana::io::ImageImmutableRange* literal_proof =
            writer_proof;
        if (writer->instruction.kind == K::MovLongLoadPcRelative) {
            const auto literal_address =
                ((writer->address + 4u) & ~3u) +
                static_cast<std::uint32_t>(writer->instruction.displacement);
            const auto resolved_literal = image.resolve_segment_address(literal_address, 4u);
            literal_proof = resolved_literal.has_value()
                                ? immutable_image_proof(*resolved_literal, false, 4u)
                                : nullptr;
            immutable_literal = literal_proof != nullptr;
        }
        const auto same_authenticated_view =
            [](const katana::io::ImageImmutableRange* left,
               const katana::io::ImageImmutableRange* right) {
                return left != nullptr && right != nullptr &&
                       left->generation == right->generation;
            };
        bool immutable_definition_sites = !result.definition_sites.empty();
        bool same_image = dispatch_proof != nullptr;
        for (const auto definition : result.definition_sites) {
            const auto* const proof =
                immutable_image_proof(definition, true, 2u);
            if (proof == nullptr) {
                immutable_definition_sites = false;
                continue;
            }
            same_image =
                same_image && same_authenticated_view(dispatch_proof, proof);
        }
        same_image = same_image &&
                     same_authenticated_view(dispatch_proof, writer_proof) &&
                     same_authenticated_view(dispatch_proof, literal_proof) &&
                     same_authenticated_view(dispatch_proof, target_proof);
        result.exact_target_guard =
            target_opcode_known &&
            (!result.preceding_call ||
             result.preceding_call_transport_abi_proven) &&
            immutable_dispatch && immutable_definition_sites &&
            immutable_literal && immutable_target && same_image;
        if (!result.exact_target_guard) {
            if (result.preceding_call &&
                !result.preceding_call_transport_abi_proven)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::PrecedingCall;
            else if (target_segment == nullptr ||
                     !target_segment->permissions.executable)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::TargetNotExecutable;
            else if (!target_opcode_known)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::UnknownTargetOpcode;
            else if (!immutable_dispatch)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::DispatchImmutableProofMissing;
            else if (!immutable_writer)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::WriterImmutableProofMissing;
            else if (!immutable_definition_sites)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::DefinitionImmutableProofMissing;
            else if (!immutable_literal)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::LiteralImmutableProofMissing;
            else if (!immutable_target)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::TargetImmutableProofMissing;
            else if (!same_image)
                result.exact_target_rejection_reason =
                    ExactGuardRejectionReason::ImageIdentityMismatch;
        }
        return result;
    }
    return result;
}

void classify_dynamic_sites(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::io::ExecutableImage& image,
    std::vector<IndirectControlFlowResolution>& resolutions,
    const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    const auto blocks = build_basic_blocks(lines, resolved_edges);
    const auto writer_slice_index = build_writer_slice_index(blocks);
    for (auto& resolution : resolutions) {
        if (resolution.origin_class != IndirectControlFlowOriginClass::Table &&
            resolution.status == ResolutionStatus::Resolved &&
            control_flow_evidence_complete(resolution.evidence) &&
            resolution.exact_target_guard)
            continue;
        resolution.exact_target_guard = false;
        resolution.exact_guard_rejection_reason =
            ExactGuardRejectionReason::None;
        const auto dispatch = std::lower_bound(
            lines.begin(),
            lines.end(),
            resolution.instruction_address,
            [](const auto& line, const auto address) { return line.address < address; });
        if (dispatch == lines.end() || dispatch->address != resolution.instruction_address) {
            resolution.origin_class = IndirectControlFlowOriginClass::RuntimePointer;
            if (resolution.evidence_origins.empty()) {
                resolution.evidence_origins = {AnalysisEvidenceOrigin::RuntimeClassification};
            }
            continue;
        }
        const auto slice = bounded_writer_slice(
            writer_slice_index, resolution.instruction_address, resolution.register_index);
        resolution.definition_sites.assign(slice.writers.begin(), slice.writers.end());
        resolution.definition_complete = !slice.path_incomplete &&
                                         !slice.writer_not_memory_load &&
                                         !slice.writers.empty();
        resolution.preceding_call = slice.preceding_call;
        const katana::sh4::DisassemblyLine* writer = nullptr;
        if (slice.writers.size() == 1u && !slice.path_incomplete) {
            writer = find_line(lines, *slice.writers.begin());
        }
        if (dispatch->instruction.kind == katana::sh4::InstructionKind::Jsr ||
            dispatch->instruction.kind == katana::sh4::InstructionKind::Jmp) {
            const auto static_chain = resolve_static_code_pointer_chain(
                writer_slice_index, lines, image, resolution.instruction_address,
                resolution.register_index);
            resolution.preceding_call = resolution.preceding_call ||
                                        static_chain.preceding_call;
            if (static_chain.complete && static_chain.target.has_value()) {
                const auto target = *static_chain.target;
                const auto contains_other_target =
                    [target](const std::vector<std::uint32_t>& values) {
                        return std::any_of(
                            values.begin(), values.end(),
                            [target](const auto value) { return value != target; });
                    };
                const bool existing_target_conflict =
                    (resolution.target.has_value() &&
                     *resolution.target != target) ||
                    contains_other_target(resolution.targets) ||
                    contains_other_target(resolution.analysis_candidates);
                resolution.definition_sites.assign(
                    static_chain.definition_sites.begin(),
                    static_chain.definition_sites.end());
                resolution.definition_complete =
                    !static_chain.preceding_call ||
                    static_chain.preceding_call_transport_abi_proven;
                resolution.analysis_candidates.push_back(target);
                std::sort(resolution.analysis_candidates.begin(),
                          resolution.analysis_candidates.end());
                resolution.analysis_candidates.erase(
                    std::unique(resolution.analysis_candidates.begin(),
                                resolution.analysis_candidates.end()),
                    resolution.analysis_candidates.end());
                if (static_chain.exact_target_guard &&
                    !existing_target_conflict &&
                    resolution.analysis_candidates.size() == 1u) {
                    // The immutable writer chain is a complete, single-target
                    // edge. Keep the candidate as provenance for the exporter,
                    // while the target itself becomes a guarded CFG edge. The
                    // emitter must reject any live mismatch rather than pass it
                    // to the global RuntimeOnly dispatcher.
                    resolution.target = target;
                    resolution.targets.clear();
                    resolution.status = ResolutionStatus::Guarded;
                    resolution.evidence = ControlFlowEvidence::GuardedComplete;
                    resolution.evidence_origins = {
                        AnalysisEvidenceOrigin::LocalValue};
                    resolution.reason =
                        "exact-immutable-pc-relative-code-pointer-chain";
                    resolution.value_source =
                        "immutable-image-literal-chain";
                    resolution.exact_target_guard = true;
                    resolution.exact_guard_rejection_reason =
                        ExactGuardRejectionReason::None;
                } else {
                    resolution.exact_guard_rejection_reason =
                        existing_target_conflict ||
                                resolution.analysis_candidates.size() != 1u
                            ? ExactGuardRejectionReason::ConflictingTargets
                            : static_chain.exact_target_rejection_reason;
                    if (resolution.status == ResolutionStatus::Unresolved) {
                        resolution.evidence = ControlFlowEvidence::RuntimeOnly;
                        resolution.evidence_origins = {
                            AnalysisEvidenceOrigin::LocalValue};
                        resolution.reason =
                            "runtime-contract-static-code-pointer-chain";
                    }
                }
            }
            if (!control_flow_evidence_complete(resolution.evidence)) {
                const auto static_set = resolve_static_code_pointer_set(
                    writer_slice_index,
                    lines,
                    image,
                    resolution.instruction_address,
                    resolution.register_index);
                if (static_set.complete && static_set.targets.size() == 2u) {
                    const auto target_outside_set =
                        [&static_set](const std::uint32_t target) {
                            return !std::binary_search(
                                static_set.targets.begin(),
                                static_set.targets.end(),
                                target);
                        };
                    const auto contains_outside_target =
                        [&target_outside_set](
                            const std::vector<std::uint32_t>& targets) {
                            return std::any_of(
                                targets.begin(),
                                targets.end(),
                                target_outside_set);
                        };
                    const bool existing_target_conflict =
                        (resolution.target.has_value() &&
                         target_outside_set(*resolution.target)) ||
                        contains_outside_target(resolution.targets) ||
                        contains_outside_target(
                            resolution.analysis_candidates);
                    resolution.definition_sites.assign(
                        static_set.definition_sites.begin(),
                        static_set.definition_sites.end());
                    resolution.definition_complete = true;
                    if (!existing_target_conflict) {
                        resolution.target.reset();
                        resolution.targets = static_set.targets;
                        resolution.analysis_candidates.insert(
                            resolution.analysis_candidates.end(),
                            static_set.targets.begin(),
                            static_set.targets.end());
                        std::sort(resolution.analysis_candidates.begin(),
                                  resolution.analysis_candidates.end());
                        resolution.analysis_candidates.erase(
                            std::unique(
                                resolution.analysis_candidates.begin(),
                                resolution.analysis_candidates.end()),
                            resolution.analysis_candidates.end());
                        resolution.status = ResolutionStatus::Guarded;
                        resolution.evidence =
                            ControlFlowEvidence::GuardedComplete;
                        resolution.evidence_origins = {
                            AnalysisEvidenceOrigin::LocalValue};
                        resolution.reason =
                            "guarded-complete-immutable-pc-relative-code-pointer-set";
                        resolution.value_source =
                            "immutable-image-literal-set";
                        resolution.exact_target_guard = false;
                        resolution.exact_guard_rejection_reason =
                            ExactGuardRejectionReason::None;
                    } else {
                        resolution.exact_guard_rejection_reason =
                            ExactGuardRejectionReason::ConflictingTargets;
                    }
                } else if (
                    static_set.exact_target_rejection_reason !=
                        ExactGuardRejectionReason::None &&
                    resolution.exact_guard_rejection_reason ==
                        ExactGuardRejectionReason::None) {
                    resolution.exact_guard_rejection_reason =
                        static_set.exact_target_rejection_reason;
                }
            }
        }
        if (resolution.origin_class == IndirectControlFlowOriginClass::Table) continue;
        bool vtable_base = false;
        bool stack_base = false;
        bool object_field = false;
        bool callback_source = resolution.register_index == 13u;
        if (writer != nullptr) {
            const auto base = bounded_writer_slice(
                writer_slice_index, writer->address, writer->instruction.source_register);
            const bool unique_base = base.writers.size() == 1u &&
                                     !base.path_incomplete;
            vtable_base = unique_base && !base.writer_not_memory_load;
            callback_source = callback_source || (writer->instruction.kind ==
                                                      katana::sh4::InstructionKind::MovRegister &&
                                                  writer->instruction.source_register == 13u);
            const auto writer_kind = writer->instruction.kind;
            object_field = writer->instruction.source_register >= 4u &&
                           writer->instruction.source_register <= 14u &&
                           (writer_kind == katana::sh4::InstructionKind::MovLongLoad ||
                            writer_kind == katana::sh4::InstructionKind::MovLongLoadDisplacement);
            if (unique_base) {
                const auto base_writer = std::lower_bound(
                    lines.begin(),
                    lines.end(),
                    *base.writers.begin(),
                    [](const auto& line, const auto address) { return line.address < address; });
                stack_base =
                    base_writer != lines.end() && base_writer->address == *base.writers.begin() &&
                    base_writer->instruction.kind == katana::sh4::InstructionKind::MovRegister &&
                    base_writer->instruction.source_register == 15u;
            }
        }
        if ((resolution.register_index == 0u && slice.preceding_call) || callback_source) {
            resolution.origin_class = IndirectControlFlowOriginClass::Callback;
        } else if (resolution.register_index == 15u ||
                   (writer != nullptr && writer->instruction.source_register == 15u) ||
                   stack_base) {
            resolution.origin_class = IndirectControlFlowOriginClass::Stack;
        } else if (writer != nullptr && (vtable_base || object_field)) {
            resolution.origin_class = IndirectControlFlowOriginClass::ObjectVTable;
        } else if (writer != nullptr) {
            resolution.origin_class = IndirectControlFlowOriginClass::UnboundedMemory;
        } else if (resolution.register_index >= 4u && resolution.register_index <= 7u) {
            resolution.origin_class = IndirectControlFlowOriginClass::Parameter;
        } else {
            resolution.origin_class = IndirectControlFlowOriginClass::RuntimePointer;
        }

        if (resolution.status == ResolutionStatus::Unresolved &&
            resolution.evidence != ControlFlowEvidence::HintCandidate &&
            resolution.evidence != ControlFlowEvidence::RuntimeOnly) {
            bool runtime_contract = false;
            switch (resolution.origin_class) {
            case IndirectControlFlowOriginClass::Callback:
                resolution.reason =
                    resolution.register_index == 13u ? "dynamic-callback" : "dynamic-return-value";
                runtime_contract = true;
                break;
            case IndirectControlFlowOriginClass::Parameter:
                resolution.reason = "dynamic-parameter";
                runtime_contract = true;
                break;
            case IndirectControlFlowOriginClass::Stack:
                resolution.reason = "dynamic-stack-target";
                runtime_contract = true;
                break;
            case IndirectControlFlowOriginClass::ObjectVTable:
                resolution.reason = "dynamic-vtable-target";
                runtime_contract = true;
                break;
            case IndirectControlFlowOriginClass::UnboundedMemory:
                resolution.reason = "dynamic-unbounded-memory";
                runtime_contract = true;
                break;
            case IndirectControlFlowOriginClass::RuntimePointer:
                if (resolution.reason.empty()) {
                    resolution.reason = "dynamic-runtime-pointer";
                } else if (!resolution.reason.starts_with("dynamic-runtime-pointer")) {
                    resolution.reason = "dynamic-runtime-pointer-" + resolution.reason;
                }
                runtime_contract = true;
                break;
            case IndirectControlFlowOriginClass::NotApplicable:
            case IndirectControlFlowOriginClass::Table:
                runtime_contract = false;
                break;
            }
            if (runtime_contract) {
                resolution.evidence = ControlFlowEvidence::RuntimeOnly;
                resolution.evidence_origins = {AnalysisEvidenceOrigin::RuntimeClassification};
            } else if (resolution.origin_class == IndirectControlFlowOriginClass::RuntimePointer &&
                       resolution.evidence_origins.empty()) {
                resolution.evidence_origins = {AnalysisEvidenceOrigin::RuntimeClassification};
            }
        } else if (resolution.evidence_origins.empty()) {
            resolution.evidence_origins = {AnalysisEvidenceOrigin::RuntimeClassification};
        }
    }
}

[[nodiscard]] bool bind_partial_runtime_contract(
    IndirectControlFlowResolution& resolution) {
        if (resolution.evidence != ControlFlowEvidence::GuardedPartial)
            return false;
        const bool guarded_memory = resolution.reason == "guarded-function-memory";
        const bool merged_contexts = resolution.reason == "merged-contexts-partial";
        const bool inventory_code_pointer_set =
            resolution.reason == "inventory-code-pointer-set";
        const bool writable_literal =
            resolution.reason.find("guarded-writable-pc-relative-literal") != std::string::npos;
        if (!guarded_memory && !merged_contexts &&
            !inventory_code_pointer_set && !writable_literal)
            return false;
        if (resolution.target.has_value())
            resolution.analysis_candidates.push_back(*resolution.target);
        resolution.analysis_candidates.insert(resolution.analysis_candidates.end(),
                                              resolution.targets.begin(),
                                              resolution.targets.end());
        std::sort(resolution.analysis_candidates.begin(), resolution.analysis_candidates.end());
        resolution.analysis_candidates.erase(std::unique(resolution.analysis_candidates.begin(),
                                                         resolution.analysis_candidates.end()),
                                             resolution.analysis_candidates.end());
        resolution.target.reset();
        resolution.targets.clear();
        resolution.status = ResolutionStatus::Unresolved;
        resolution.evidence = ControlFlowEvidence::RuntimeOnly;
        if (std::find(resolution.evidence_origins.begin(),
                      resolution.evidence_origins.end(),
                      AnalysisEvidenceOrigin::RuntimeClassification) ==
            resolution.evidence_origins.end())
            resolution.evidence_origins.push_back(AnalysisEvidenceOrigin::RuntimeClassification);
        resolution.reason = guarded_memory    ? "runtime-contract-function-memory"
                            : merged_contexts ? "runtime-contract-merged-contexts"
                            : inventory_code_pointer_set
                                ? "runtime-contract-inventory-code-pointer-set"
                                              : "runtime-contract-writable-literal";
        return true;
}

void bind_partial_runtime_contracts(
    std::vector<IndirectControlFlowResolution>& resolutions,
    std::set<std::uint32_t>* const changed_sites = nullptr) {
    for (auto& resolution : resolutions) {
        if (bind_partial_runtime_contract(resolution) &&
            changed_sites != nullptr)
            changed_sites->insert(resolution.instruction_address);
    }
}

void bind_partial_runtime_contracts(
    std::map<std::uint32_t, IndirectControlFlowResolution>& resolutions,
    std::set<std::uint32_t>* const changed_sites = nullptr,
    std::size_t* const visited_entries = nullptr) {
    for (auto& [site, resolution] : resolutions) {
        if (visited_entries != nullptr) ++*visited_entries;
        if (bind_partial_runtime_contract(resolution) &&
            changed_sites != nullptr)
            changed_sites->insert(site);
    }
}

void bind_partial_runtime_contracts(
    std::map<std::uint32_t, IndirectControlFlowResolution>& resolutions,
    const std::set<std::uint32_t>& selected_sites,
    std::set<std::uint32_t>* const changed_sites,
    std::size_t* const visited_entries) {
    for (const auto site : selected_sites) {
        const auto resolution = resolutions.find(site);
        if (resolution == resolutions.end()) continue;
        if (visited_entries != nullptr) ++*visited_entries;
        if (bind_partial_runtime_contract(resolution->second) &&
            changed_sites != nullptr)
            changed_sites->insert(site);
    }
}

std::vector<ResolvedControlFlowEdge>
collect_resolved_edges(const std::span<const IndirectControlFlowResolution> resolutions,
                       const std::span<const JumpTableAnalysis> tables) {
    std::vector<ResolvedControlFlowEdge> edges;
    for (const auto& resolution : resolutions) {
        std::vector<std::uint32_t> targets = resolution.targets;
        if (resolution.target.has_value()) targets.push_back(*resolution.target);
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (targets.empty()) continue;
        for (const auto target : targets) {
            edges.push_back({resolution.instruction_address,
                             target,
                             resolution.kind == IndirectControlFlowKind::Call
                                 ? ResolvedControlFlowKind::Call
                                 : ResolvedControlFlowKind::Jump,
                             resolution.evidence != ControlFlowEvidence::ProvenComplete,
                             resolution.evidence,
                             resolution.evidence_origins});
        }
    }
    for (const auto& table : tables) {
        if (!table.resolved || table.aot_candidates_only) continue;
        for (const auto& entry : table.entries) {
            edges.push_back({table.dispatch_address,
                             entry.target,
                             table.dispatch_kind == JumpTableDispatchKind::Call
                                 ? ResolvedControlFlowKind::Call
                                 : ResolvedControlFlowKind::Jump,
                             table.evidence != ControlFlowEvidence::ProvenComplete,
                             table.evidence,
                             {table.evidence == ControlFlowEvidence::HintCandidate
                                  ? AnalysisEvidenceOrigin::UserHint
                              : table.evidence == ControlFlowEvidence::ForcedOverride
                                  ? AnalysisEvidenceOrigin::UserOverride
                                  : AnalysisEvidenceOrigin::JumpTable}});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const auto& left, const auto& right) {
        if (left.instruction_address != right.instruction_address)
            return left.instruction_address < right.instruction_address;
        if (left.target_address != right.target_address)
            return left.target_address < right.target_address;
        if (left.kind != right.kind) return left.kind < right.kind;
        if (left.guarded != right.guarded) return left.guarded < right.guarded;
        if (left.evidence != right.evidence) return left.evidence < right.evidence;
        return left.evidence_origins < right.evidence_origins;
    });
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}

struct FunctionEdgeFamilies final {
    std::vector<ResolvedControlFlowEdge> semantic;
    std::vector<ResolvedControlFlowEdge> candidate_calls;
    std::vector<ResolvedControlFlowEdge> candidate_tails;

    bool operator==(const FunctionEdgeFamilies&) const = default;
};

struct FunctionEdgeJournalStateRoot final {
    std::shared_ptr<const FunctionEdgeJournalStateRoot> base;
    std::map<std::uint32_t, std::optional<FunctionEdgeFamilies>> changes;
};

[[nodiscard]] bool function_edge_less(
    const ResolvedControlFlowEdge& left,
    const ResolvedControlFlowEdge& right) noexcept {
    return std::tie(left.instruction_address, left.target_address,
                    left.kind, left.guarded, left.evidence,
                    left.evidence_origins,
                    left.analysis_candidate_carrier) <
           std::tie(right.instruction_address, right.target_address,
                    right.kind, right.guarded, right.evidence,
                    right.evidence_origins,
                    right.analysis_candidate_carrier);
}

void canonicalize_function_edges(
    std::vector<ResolvedControlFlowEdge>& edges) {
    std::sort(edges.begin(), edges.end(), function_edge_less);
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
}

class FunctionEdgeJournal final {
  public:
    void clear() {
        families_.clear();
        pending_semantic_.clear();
        pending_candidate_calls_.clear();
        pending_candidate_tails_.clear();
        state_root_.reset();
        fingerprint_a_ = 0u;
        fingerprint_b_ = 0u;
        state_entry_count_ = 0u;
    }
    void refresh(
        const std::set<std::uint32_t>& sites,
        const std::map<std::uint32_t, IndirectControlFlowResolution>&
            resolutions,
        const std::map<std::uint32_t, JumpTableAnalysis>& tables,
        ControlFlowAnalysisResult& telemetry) {
        std::map<std::uint32_t, std::optional<FunctionEdgeFamilies>>
            state_changes;
        for (const auto site : sites) {
            ++telemetry.function_edge_family_entries_visited;
            FunctionEdgeFamilies next;
            const auto resolution_index = resolutions.find(site);
            if (resolution_index != resolutions.end()) {
                const auto& resolution = resolution_index->second;
                auto targets = resolution.targets;
                if (resolution.target.has_value())
                    targets.push_back(*resolution.target);
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()),
                              targets.end());
                for (const auto target : targets) {
                    next.semantic.push_back(
                        {site,
                         target,
                         resolution.kind == IndirectControlFlowKind::Call
                             ? ResolvedControlFlowKind::Call
                             : ResolvedControlFlowKind::Jump,
                         resolution.evidence !=
                             ControlFlowEvidence::ProvenComplete,
                         resolution.evidence,
                         resolution.evidence_origins});
                }
                if ((resolution.kind == IndirectControlFlowKind::Call ||
                     resolution.kind == IndirectControlFlowKind::Jump) &&
                    !(resolution.kind == IndirectControlFlowKind::Jump &&
                      resolution.origin_class ==
                          IndirectControlFlowOriginClass::Table)) {
                    auto& candidates =
                        resolution.kind == IndirectControlFlowKind::Call
                            ? next.candidate_calls
                            : next.candidate_tails;
                    for (const auto target :
                         resolution.analysis_candidates) {
                        candidates.push_back(
                            {site,
                             target,
                             ResolvedControlFlowKind::Call,
                             true,
                             ControlFlowEvidence::GuardedPartial,
                             resolution.evidence_origins,
                             true});
                    }
                }
            }
            const auto table_index = tables.find(site);
            if (table_index != tables.end()) {
                const auto& table = table_index->second;
                if (table.resolved && !table.aot_candidates_only) {
                for (const auto& entry : table.entries) {
                    next.semantic.push_back(
                        {site,
                         entry.target,
                         table.dispatch_kind == JumpTableDispatchKind::Call
                             ? ResolvedControlFlowKind::Call
                             : ResolvedControlFlowKind::Jump,
                         table.evidence !=
                             ControlFlowEvidence::ProvenComplete,
                         table.evidence,
                         {table.evidence ==
                                  ControlFlowEvidence::HintCandidate
                              ? AnalysisEvidenceOrigin::UserHint
                          : table.evidence ==
                                  ControlFlowEvidence::ForcedOverride
                              ? AnalysisEvidenceOrigin::UserOverride
                              : AnalysisEvidenceOrigin::JumpTable}});
                }
                }
            }
            canonicalize_function_edges(next.semantic);
            canonicalize_function_edges(next.candidate_calls);
            canonicalize_function_edges(next.candidate_tails);
            const auto previous = families_.find(site);
            const FunctionEdgeFamilies empty;
            const auto& old = previous == families_.end()
                                  ? empty
                                  : previous->second;
            if (old == next) continue;
            const auto old_token = state_token(site, old, telemetry);
            const auto next_token = state_token(site, next, telemetry);
            fingerprint_a_ ^= old_token.first ^ next_token.first;
            fingerprint_b_ ^= old_token.second ^ next_token.second;
            if (old == empty && !(next == empty)) ++state_entry_count_;
            if (!(old == empty) && next == empty) --state_entry_count_;
            stage_family_delta(site, old.semantic, next.semantic,
                               pending_semantic_);
            stage_family_delta(site, old.candidate_calls,
                               next.candidate_calls,
                               pending_candidate_calls_);
            stage_family_delta(site, old.candidate_tails,
                               next.candidate_tails,
                               pending_candidate_tails_);
            if (next == empty) {
                families_.erase(site);
                state_changes.emplace(site, std::nullopt);
            } else {
                state_changes.emplace(site, next);
                families_.insert_or_assign(site, std::move(next));
            }
            ++telemetry.function_edge_family_entries_rebuilt;
        }
        if (!state_changes.empty()) {
            auto root = std::make_shared<FunctionEdgeJournalStateRoot>();
            root->base = state_root_;
            root->changes = std::move(state_changes);
            for (const auto& [site, value] : root->changes) {
                static_cast<void>(site);
                ++telemetry.function_edge_state_copy_items;
                if (value.has_value())
                    telemetry.function_edge_state_copy_items +=
                        value->semantic.size() +
                        value->candidate_calls.size() +
                        value->candidate_tails.size();
            }
            state_root_ = std::move(root);
        }
    }

    [[nodiscard]] std::vector<ResolvedControlFlowEdge>
    materialize_all() const {
        std::vector<ResolvedControlFlowEdge> result;
        for (const auto& [site, family] : families_) {
            static_cast<void>(site);
            result.insert(result.end(), family.semantic.begin(),
                          family.semantic.end());
            result.insert(result.end(), family.candidate_calls.begin(),
                          family.candidate_calls.end());
            result.insert(result.end(), family.candidate_tails.begin(),
                          family.candidate_tails.end());
        }
        return result;
    }

    void append_pending_delta(detail::FunctionProgramDelta& delta) const {
        append_delta_map(pending_semantic_,
                         delta.changed_semantic_edge_sites);
        append_delta_map(pending_candidate_calls_,
                         delta.changed_candidate_call_sites);
        append_delta_map(pending_candidate_tails_,
                         delta.changed_candidate_tail_sites);
    }

    void clear_pending_delta() {
        pending_semantic_.clear();
        pending_candidate_calls_.clear();
        pending_candidate_tails_.clear();
    }

    [[nodiscard]] bool pending_delta_empty() const noexcept {
        return pending_semantic_.empty() &&
               pending_candidate_calls_.empty() &&
               pending_candidate_tails_.empty();
    }

    [[nodiscard]] const std::map<std::uint32_t, FunctionEdgeFamilies>&
    families() const noexcept {
        return families_;
    }

    [[nodiscard]] std::shared_ptr<const FunctionEdgeJournalStateRoot>
    state_root() const noexcept {
        return state_root_;
    }

    [[nodiscard]] auto state_fingerprint() const noexcept {
        return std::tuple{fingerprint_a_, fingerprint_b_,
                          state_entry_count_};
    }

    [[nodiscard]] static bool exact_state_equal(
        const std::shared_ptr<const FunctionEdgeJournalStateRoot>& left,
        const std::shared_ptr<const FunctionEdgeJournalStateRoot>& right,
        ControlFlowAnalysisResult& telemetry) {
        if (left == right) return true;
        const auto flatten = [&](auto root) {
            std::vector<const FunctionEdgeJournalStateRoot*> chain;
            while (root != nullptr) {
                chain.push_back(root.get());
                root = root->base;
            }
            std::reverse(chain.begin(), chain.end());
            std::map<std::uint32_t, FunctionEdgeFamilies> values;
            for (const auto* layer : chain) {
                for (const auto& [site, value] : layer->changes) {
                    ++telemetry.function_edge_state_exact_compare_items;
                    if (value.has_value())
                        values.insert_or_assign(site, *value);
                    else
                        values.erase(site);
                }
            }
            return values;
        };
        return flatten(left) == flatten(right);
    }

  private:
    using DeltaMap =
        std::map<std::uint32_t, detail::FunctionProgramEdgeSiteDelta>;

    static void stage_family_delta(
        const std::uint32_t site,
        const std::vector<ResolvedControlFlowEdge>& old,
        const std::vector<ResolvedControlFlowEdge>& next,
        DeltaMap& pending) {
        if (old == next) return;
        detail::FunctionProgramEdgeSiteDelta delta;
        delta.instruction_address = site;
        delta.values = next;
        pending.insert_or_assign(site, std::move(delta));
    }

    static void append_delta_map(
        const DeltaMap& source,
        std::vector<detail::FunctionProgramEdgeSiteDelta>& destination) {
        destination.reserve(destination.size() + source.size());
        for (const auto& [site, delta] : source) {
            static_cast<void>(site);
            destination.push_back(delta);
        }
    }

    static std::pair<std::uint64_t, std::uint64_t> state_token(
        const std::uint32_t site,
        const FunctionEdgeFamilies& family,
        ControlFlowAnalysisResult& telemetry) {
        if (family == FunctionEdgeFamilies{}) return {0u, 0u};
        std::string encoded;
        const auto append = [&encoded](const auto value) {
            using Value = std::remove_cv_t<decltype(value)>;
            auto bits = static_cast<std::uint64_t>(value);
            for (std::size_t index = 0u; index < sizeof(Value); ++index) {
                encoded.push_back(static_cast<char>(bits & 0xFFu));
                bits >>= 8u;
            }
        };
        append(site);
        const auto append_family = [&](const auto& edges) {
            append(static_cast<std::uint64_t>(edges.size()));
            for (const auto& edge : edges) {
                ++telemetry.function_edge_state_encode_items;
                append(edge.instruction_address);
                append(edge.target_address);
                append(edge.kind);
                append(static_cast<std::uint8_t>(edge.guarded));
                append(edge.evidence);
                append(static_cast<std::uint8_t>(
                    edge.analysis_candidate_carrier));
                append(static_cast<std::uint64_t>(
                    edge.evidence_origins.size()));
                for (const auto origin : edge.evidence_origins)
                    append(origin);
            }
        };
        ++telemetry.function_edge_state_encode_items;
        append_family(family.semantic);
        append_family(family.candidate_calls);
        append_family(family.candidate_tails);
        const auto digest = katana::io::sha256_bytes(encoded);
        const auto parse = [&](const std::size_t offset) {
            std::uint64_t result = 0u;
            for (std::size_t index = 0u; index < 16u; ++index) {
                const auto digit = digest[offset + index];
                const auto value = digit >= '0' && digit <= '9'
                                       ? digit - '0'
                                       : digit - 'a' + 10;
                result = (result << 4u) |
                         static_cast<std::uint64_t>(value);
            }
            return result;
        };
        return {parse(0u), parse(16u)};
    }

    std::map<std::uint32_t, FunctionEdgeFamilies> families_;
    DeltaMap pending_semantic_;
    DeltaMap pending_candidate_calls_;
    DeltaMap pending_candidate_tails_;
    std::shared_ptr<const FunctionEdgeJournalStateRoot> state_root_;
    std::uint64_t fingerprint_a_ = 0u;
    std::uint64_t fingerprint_b_ = 0u;
    std::size_t state_entry_count_ = 0u;
};

class CandidateContractCycleLedger final {
  public:
    [[nodiscard]] bool already_seen(
        const bool boundary_contracts_active,
        const FunctionEdgeJournal& journal,
        ControlFlowAnalysisResult& telemetry) {
        const auto [first, second, entries] =
            journal.state_fingerprint();
        const Key key{boundary_contracts_active, first, second, entries};
        const auto root = journal.state_root();
        auto& collision_bucket = states_[key];
        for (const auto& prior : collision_bucket) {
            if (FunctionEdgeJournal::exact_state_equal(
                    prior, root, telemetry))
                return true;
        }
        collision_bucket.push_back(root);
        ++size_;
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

  private:
    using Key = std::tuple<bool, std::uint64_t, std::uint64_t,
                           std::size_t>;
    std::map<Key,
             std::vector<std::shared_ptr<
                 const FunctionEdgeJournalStateRoot>>>
        states_;
    std::size_t size_ = 0u;
};

struct GuardedAotEntryCollection {
    std::vector<GuardedAotEntry> entries;
    std::vector<GuardedAotEntryRejection> rejections;
};

GuardedAotEntryRejectionReason
guarded_aot_code_address_rejection_reason(
    const CodeAddressStatus status) noexcept {
    switch (status) {
    case CodeAddressStatus::OddAddress:
        return GuardedAotEntryRejectionReason::OddAddress;
    case CodeAddressStatus::OutsideSegments:
        return GuardedAotEntryRejectionReason::OutsideSegments;
    case CodeAddressStatus::NotCodeSegment:
        return GuardedAotEntryRejectionReason::NotCodeSegment;
    case CodeAddressStatus::NotExecutableSegment:
        return GuardedAotEntryRejectionReason::NotExecutableSegment;
    case CodeAddressStatus::OutsideCommittedData:
        return GuardedAotEntryRejectionReason::OutsideCommittedData;
    case CodeAddressStatus::Valid:
        break;
    }
    return GuardedAotEntryRejectionReason::OutsideSegments;
}

GuardedAotEntryCollection
collect_guarded_aot_entries(const katana::io::ExecutableImage& image,
                            const ControlFlowAnalysisResult& analysis,
                            const GuardedCodeInventory& guarded_code_inventory) {
    std::map<std::uint32_t, GuardedAotEntry> entries;
    std::map<std::pair<std::uint32_t, GuardedAotEntryRejectionReason>,
             GuardedAotEntryRejection>
        rejections;
    std::map<const katana::io::ImageSegment*, std::string> source_identities;
    const auto add_entry =
        [&](const std::uint32_t address,
            const GuardedAotEntryOrigin origin,
            const std::span<const std::uint32_t> source_sites,
            const std::span<const std::uint32_t> source_objects = {}) {
            const auto reject =
                [&](const std::uint32_t resolved_address,
                    const GuardedAotEntryRejectionReason reason) {
                    auto& rejection = rejections[{address, reason}];
                    rejection.guest_address = address;
                    rejection.resolved_address = resolved_address;
                    rejection.reason = reason;
                    rejection.evidence =
                        ControlFlowEvidence::GuardedPartial;
                    rejection.origins.push_back(origin);
                    rejection.source_sites.insert(rejection.source_sites.end(),
                                                  source_sites.begin(),
                                                  source_sites.end());
                    rejection.source_objects.insert(
                        rejection.source_objects.end(),
                        source_objects.begin(),
                        source_objects.end());
                };
            const auto validation = validate_committed_code_address(image, address);
            if (!validation.valid()) {
                reject(validation.resolved_address,
                       guarded_aot_code_address_rejection_reason(
                           validation.status));
                return;
            }
            const auto resolved = validation.resolved_address;
            const auto* line = find_instruction(analysis.recursive, resolved);
            if (line == nullptr) {
                reject(resolved,
                       GuardedAotEntryRejectionReason::
                           InstructionNotAnalyzed);
                return;
            }
            if (line->is_delay_slot) {
                reject(resolved,
                       GuardedAotEntryRejectionReason::DelaySlotEntry);
                return;
            }
            if (!line->instruction.is_known()) {
                reject(resolved,
                       GuardedAotEntryRejectionReason::UnknownInstruction);
                return;
            }
            const auto entry_extent =
                line->instruction.has_delay_slot ? 4u : 2u;
            const auto* segment = image.find_segment(resolved, entry_extent);
            if (segment == nullptr) {
                reject(resolved,
                       GuardedAotEntryRejectionReason::
                           EntryExtentUnavailable);
                return;
            }
            const auto source_offset = segment->source_byte_offset(resolved);
            const auto byte_offset = segment->byte_offset(resolved);
            if (!source_offset.has_value()) {
                reject(resolved,
                       GuardedAotEntryRejectionReason::
                           SourceByteOffsetUnavailable);
                return;
            }
            if (!byte_offset.has_value()) {
                reject(resolved,
                       GuardedAotEntryRejectionReason::
                           SegmentByteOffsetUnavailable);
                return;
            }
            if (*byte_offset > segment->bytes.size() ||
                entry_extent > segment->bytes.size() - *byte_offset) {
                reject(resolved,
                       GuardedAotEntryRejectionReason::
                           EntryBytesUnavailable);
                return;
            }
            auto& entry = entries[resolved];
            if (entry.source_identity.empty()) {
                auto& identity = source_identities[segment];
                if (identity.empty()) {
                    identity =
                        "sha256:" +
                        katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(segment->bytes.data()),
                            segment->bytes.size()));
                }
                entry.guest_address = resolved;
                entry.shared_body_address = resolved;
                entry.evidence = ControlFlowEvidence::GuardedPartial;
                entry.source_identity = identity;
                entry.source_byte_offset = *source_offset;
                entry.entry_byte_extent = entry_extent;
                entry.entry_byte_identity =
                    "sha256:" +
                    katana::io::sha256_bytes(std::string_view(
                        reinterpret_cast<const char*>(
                            segment->bytes.data() + *byte_offset),
                        entry_extent));
                if (line->instruction.control_flow ==
                        katana::sh4::ControlFlowKind::UnconditionalBranch &&
                    line->target_address.has_value() &&
                    find_instruction(analysis.recursive,
                                     *line->target_address) != nullptr)
                    entry.shared_body_address = *line->target_address;
            }
            entry.origins.push_back(origin);
            entry.source_sites.insert(
                entry.source_sites.end(), source_sites.begin(), source_sites.end());
            entry.source_objects.insert(
                entry.source_objects.end(),
                source_objects.begin(),
                source_objects.end());
        };

    for (const auto& resolution : analysis.indirect_control_flow) {
        auto targets = resolution.analysis_candidates;
        if (!control_flow_evidence_proven(resolution.evidence)) {
            if (resolution.target.has_value())
                targets.push_back(*resolution.target);
            targets.insert(
                targets.end(), resolution.targets.begin(), resolution.targets.end());
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        const auto origin =
            resolution.kind == IndirectControlFlowKind::Call
                ? GuardedAotEntryOrigin::IndirectCall
                : GuardedAotEntryOrigin::TailIngress;
        const std::array source_sites{resolution.instruction_address};
        for (const auto target : targets)
            add_entry(target, origin, source_sites);
    }
    for (const auto& continuation : analysis.static_return_continuations) {
        if (!control_flow_evidence_proven(continuation.evidence)) {
            const std::array source_sites{continuation.instruction_address};
            add_entry(continuation.target_address,
                      GuardedAotEntryOrigin::StaticReturn,
                      source_sites);
        }
    }
    for (const auto& table : analysis.jump_tables) {
        if (table.dispatch_kind != JumpTableDispatchKind::Jump ||
            (control_flow_evidence_proven(table.evidence) &&
             !table.aot_candidates_only))
            continue;
        for (const auto& entry : table.entries) {
            if (entry.accepted) {
                const std::array source_sites{table.dispatch_address};
                const std::array source_objects{table.table_address};
                add_entry(entry.target,
                          GuardedAotEntryOrigin::JumpTableTail,
                          source_sites,
                          source_objects);
            }
        }
    }
    for (const auto& candidate :
         guarded_code_inventory.stored_code_addresses) {
        auto source_sites = candidate.store_instruction_addresses;
        source_sites.insert(source_sites.end(),
                            candidate.evidence_call_sites.begin(),
                            candidate.evidence_call_sites.end());
        add_entry(candidate.target_address,
                  GuardedAotEntryOrigin::StoredCodeAddress,
                  source_sites,
                  candidate.evidence_callees);
    }
    for (const auto& table :
         guarded_code_inventory.returned_code_address_tables) {
        auto source_sites = table.load_instruction_addresses;
        source_sites.insert(source_sites.end(),
                            table.evidence_call_sites.begin(),
                            table.evidence_call_sites.end());
        auto source_objects = table.evidence_callees;
        source_objects.push_back(table.table_address);
        for (const auto target : table.target_addresses)
            add_entry(target,
                      GuardedAotEntryOrigin::ReturnedCodeAddressTable,
                      source_sites,
                      source_objects);
    }
    GuardedAotEntryCollection result;
    result.entries.reserve(entries.size());
    for (auto& [address, entry] : entries) {
        static_cast<void>(address);
        std::sort(entry.origins.begin(), entry.origins.end());
        entry.origins.erase(
            std::unique(entry.origins.begin(), entry.origins.end()),
            entry.origins.end());
        std::sort(entry.source_sites.begin(), entry.source_sites.end());
        entry.source_sites.erase(
            std::unique(entry.source_sites.begin(), entry.source_sites.end()),
            entry.source_sites.end());
        std::sort(entry.source_objects.begin(), entry.source_objects.end());
        entry.source_objects.erase(
            std::unique(entry.source_objects.begin(), entry.source_objects.end()),
            entry.source_objects.end());
        result.entries.push_back(std::move(entry));
    }
    result.rejections.reserve(rejections.size());
    for (auto& [key, rejection] : rejections) {
        static_cast<void>(key);
        std::sort(rejection.origins.begin(), rejection.origins.end());
        rejection.origins.erase(
            std::unique(rejection.origins.begin(), rejection.origins.end()),
            rejection.origins.end());
        std::sort(rejection.source_sites.begin(),
                  rejection.source_sites.end());
        rejection.source_sites.erase(
            std::unique(rejection.source_sites.begin(),
                        rejection.source_sites.end()),
            rejection.source_sites.end());
        std::sort(rejection.source_objects.begin(),
                  rejection.source_objects.end());
        rejection.source_objects.erase(
            std::unique(rejection.source_objects.begin(),
                        rejection.source_objects.end()),
            rejection.source_objects.end());
        result.rejections.push_back(std::move(rejection));
    }
    return result;
}

} // namespace

namespace {

constexpr std::size_t maximum_control_flow_session_binding_entries =
    65'536u;
constexpr std::size_t maximum_control_flow_session_binding_string_bytes =
    16u * 1024u * 1024u;
constexpr std::size_t maximum_control_flow_session_seed_causes =
    1u * 1024u * 1024u;
constexpr std::size_t maximum_control_flow_session_seed_evidence_items =
    4u * 1024u * 1024u;

struct ControlFlowSessionOptionsBinding final {
    bool detailed_cache_miss_telemetry = false;
    bool enable_function_value_analysis = true;
    std::size_t maximum_fixpoint_iterations = 0u;
    std::size_t maximum_instructions = 0u;
    std::size_t maximum_contexts = 0u;
    AnalysisMemoryBudget* pre_reserved_function_value_ready_budget = nullptr;
    AnalysisMemoryBudget* function_value_cache_memory_budget = nullptr;

    bool operator==(const ControlFlowSessionOptionsBinding&) const = default;
};

[[nodiscard]] ControlFlowSessionOptionsBinding
make_control_flow_session_options_binding(
    const ControlFlowAnalysisOptions& options) noexcept {
    return {options.detailed_cache_miss_telemetry,
            options.enable_function_value_analysis,
            options.maximum_fixpoint_iterations,
            options.maximum_instructions,
            options.maximum_contexts,
            options.pre_reserved_function_value_ready_budget,
            options.function_value_cache_memory_budget};
}

[[nodiscard]] bool same_seed_evidence(const SeedEvidence& left,
                                      const SeedEvidence& right) {
    return left.origins == right.origins &&
           left.proven == right.proven &&
           left.evidence == right.evidence &&
           left.function_size == right.function_size &&
           left.causes == right.causes;
}

[[nodiscard]] bool control_flow_session_seed_binding_is_bounded(
    const std::map<std::uint32_t, SeedEvidence>& seeds) noexcept {
    if (seeds.size() > maximum_control_flow_session_binding_entries)
        return false;
    std::size_t causes = 0u;
    std::size_t evidence_items = 0u;
    for (const auto& [address, seed] : seeds) {
        static_cast<void>(address);
        if (seed.causes.size() >
            maximum_control_flow_session_seed_causes -
                std::min(causes,
                         maximum_control_flow_session_seed_causes))
            return false;
        causes += seed.causes.size();
        for (const auto& cause : seed.causes) {
            const std::array item_counts{
                cause.evidence_call_sites.size(),
                cause.evidence_callees.size()};
            for (const auto items : item_counts) {
                if (items >
                    maximum_control_flow_session_seed_evidence_items -
                        std::min(
                            evidence_items,
                            maximum_control_flow_session_seed_evidence_items))
                    return false;
                evidence_items += items;
            }
        }
    }
    return true;
}

void merge_retained_seed_evidence(
    std::map<std::uint32_t, SeedEvidence>& destination,
    const std::map<std::uint32_t, SeedEvidence>& retained) {
    for (const auto& [address, evidence] : retained) {
        std::vector<FunctionOrigin> origins(
            evidence.origins.begin(), evidence.origins.end());
        if (evidence.causes.empty()) {
            static_cast<void>(add_seed(
                destination, address, origins, evidence.proven,
                evidence.evidence, evidence.function_size));
            continue;
        }
        bool first = true;
        for (const auto& cause : evidence.causes) {
            static_cast<void>(add_seed(
                destination, address,
                first ? std::span<const FunctionOrigin>{origins}
                      : std::span<const FunctionOrigin>{},
                evidence.proven, evidence.evidence,
                evidence.function_size, cause));
            first = false;
        }
    }
}

[[nodiscard]] bool same_analysis_overrides(
    const std::optional<AnalysisOverrides>& left,
    const AnalysisOverrides* const right) {
    if (!left.has_value() || right == nullptr)
        return left.has_value() == (right != nullptr);
    if (left->version != right->version ||
        left->mode != right->mode ||
        left->source_path != right->source_path ||
        left->functions.size() != right->functions.size() ||
        left->function_boundaries.size() !=
            right->function_boundaries.size() ||
        left->function_entry_hints.size() !=
            right->function_entry_hints.size() ||
        left->jumps.size() != right->jumps.size() ||
        left->jump_tables.size() != right->jump_tables.size())
        return false;
    const auto same_functions = std::equal(
        left->functions.begin(), left->functions.end(),
        right->functions.begin(), right->functions.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.address, a.line, a.size) ==
                   std::tie(b.address, b.line, b.size);
        });
    const auto same_boundaries = std::equal(
        left->function_boundaries.begin(),
        left->function_boundaries.end(),
        right->function_boundaries.begin(),
        right->function_boundaries.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.address, a.line, a.size) ==
                   std::tie(b.address, b.line, b.size);
        });
    const auto same_entry_hints = std::equal(
        left->function_entry_hints.begin(),
        left->function_entry_hints.end(),
        right->function_entry_hints.begin(),
        right->function_entry_hints.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.address, a.line) ==
                   std::tie(b.address, b.line);
        });
    const auto same_jumps = std::equal(
        left->jumps.begin(), left->jumps.end(),
        right->jumps.begin(), right->jumps.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.instruction_address, a.target, a.line) ==
                   std::tie(b.instruction_address, b.target, b.line);
        });
    const auto same_tables = std::equal(
        left->jump_tables.begin(), left->jump_tables.end(),
        right->jump_tables.begin(), right->jump_tables.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.dispatch_address, a.table_address,
                            a.entry_count, a.line, a.entry_stride,
                            a.relative_base, a.encoding, a.transfer,
                            a.require_dispatch,
                            a.identity_bound_complete) ==
                   std::tie(b.dispatch_address, b.table_address,
                            b.entry_count, b.line, b.entry_stride,
                            b.relative_base, b.encoding, b.transfer,
                            b.require_dispatch,
                            b.identity_bound_complete);
        });
    return same_functions && same_boundaries && same_entry_hints &&
           same_jumps && same_tables;
}

[[nodiscard]] bool control_flow_session_binding_is_bounded(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* const overrides) noexcept {
    const auto immutable_ranges = image.immutable_ranges();
    std::size_t entry_count = immutable_ranges.size();
    std::size_t string_bytes = 0u;
    const auto absorb_string_bytes = [&](const std::size_t bytes) {
        if (bytes > maximum_control_flow_session_binding_string_bytes ||
            string_bytes >
                maximum_control_flow_session_binding_string_bytes - bytes)
            return false;
        string_bytes += bytes;
        return true;
    };
    for (const auto& range : immutable_ranges) {
        if (!absorb_string_bytes(range.identity.size())) return false;
    }
    if (overrides != nullptr) {
        const std::array counts{
            overrides->functions.size(),
            overrides->function_boundaries.size(),
            overrides->function_entry_hints.size(),
            overrides->jumps.size(),
            overrides->jump_tables.size()};
        for (const auto count : counts) {
            if (count > maximum_control_flow_session_binding_entries -
                            std::min(entry_count,
                                     maximum_control_flow_session_binding_entries))
                return false;
            entry_count += count;
        }
        const auto source_path_size =
            overrides->source_path.native().size();
        if (source_path_size >
            maximum_control_flow_session_binding_string_bytes /
                sizeof(std::filesystem::path::value_type))
            return false;
        if (!absorb_string_bytes(
                source_path_size *
                sizeof(std::filesystem::path::value_type)))
            return false;
    }
    return entry_count <= maximum_control_flow_session_binding_entries &&
           string_bytes <=
               maximum_control_flow_session_binding_string_bytes;
}

} // namespace

struct ControlFlowAnalysisSession::Impl final {
    bool active = false;
    bool reusable = false;
    bool candidate_binding_is_bounded = false;
    std::uint64_t image_identity = 0u;
    std::uint64_t image_revision = 0u;
    std::uint64_t image_immutable_generation = 0u;
    std::vector<katana::io::ImageImmutableRange> image_immutable_ranges;
    std::optional<AnalysisOverrides> overrides;
    ControlFlowSessionOptionsBinding options;
    std::map<std::uint32_t, SeedEvidence> root_seed_baseline;
    std::map<std::uint32_t, SeedEvidence> working_seed_baseline;
    std::map<std::uint32_t, SeedEvidence> candidate_root_seed_baseline;
    std::map<std::uint32_t, SeedEvidence> candidate_working_seed_baseline;

    std::unique_ptr<detail::RecursiveAnalysisSession> recursive_session;
    RecursiveWorkingIndex recursive_index;
    std::unique_ptr<detail::GuardedNativeEntryShapeCache>
        guarded_native_entry_shapes;
    std::unique_ptr<detail::FunctionValueAnalysisSession>
        function_value_session;
    JumpTableSnapshotCache jump_table_cache;
    IncrementalCfaScanCache cfa_scan_cache;
    std::map<std::uint32_t, FunctionBoundary> function_boundary_index;
    bool function_value_program_initialized = false;
    bool function_value_full_program_required = false;
    FunctionEdgeJournal function_edge_journal;
    bool decode_boundary_normalization_initialized = false;
    std::map<FunctionValueDependencyNodeId, FunctionValueSummary>
        function_summary_shards;
    std::map<FunctionValueDependencyNodeId,
             std::vector<InterproceduralTargetResolution>>
        function_resolution_shards;
    std::map<std::uint32_t,
             std::map<FunctionValueDependencyNodeId,
                      InterproceduralTargetResolution>>
        function_resolution_proofs_by_site;
    std::map<FunctionValueDependencyNodeId, GuardedCodeInventory>
        function_inventory_shards;
    std::map<std::pair<std::uint32_t, std::uint32_t>, RuntimeCodeCopy>
        runtime_copy_result_index;
    std::map<std::pair<std::uint32_t, std::uint32_t>,
             StaticReturnContinuationCandidate>
        continuation_result_index;
    std::map<std::uint32_t, IndirectControlFlowResolution>
        resolution_result_index;
    std::map<std::uint32_t, JumpTableAnalysis> jump_table_result_index;
    std::set<std::uint32_t> function_value_resolution_overlay_sites;
    std::set<std::uint32_t> snapshot_candidate_dispatch_index;

    void clear_working_state(const katana::io::ExecutableImage& image,
                             const ControlFlowAnalysisOptions& next_options) {
        recursive_session =
            std::make_unique<detail::RecursiveAnalysisSession>();
        recursive_index.clear();
        guarded_native_entry_shapes =
            std::make_unique<detail::GuardedNativeEntryShapeCache>(image);
        function_value_session =
            std::make_unique<detail::FunctionValueAnalysisSession>(
                16'384u,
                1'024u * 1024u * 1024u,
                next_options.detailed_cache_miss_telemetry,
                detail::FunctionEvaluationCacheDecisionObserver{},
                detail::FunctionValueAnalysisSession::
                    default_maximum_resolution_dependency_nodes,
                detail::FunctionValueAnalysisSession::
                    default_maximum_resolution_root_artifacts,
                detail::FunctionValueAnalysisSession::
                    default_maximum_resolution_epoch_retained_bytes,
                next_options.pre_reserved_function_value_ready_budget,
                next_options.function_value_cache_memory_budget);
        jump_table_cache = JumpTableSnapshotCache{};
        cfa_scan_cache.clear();
        function_boundary_index.clear();
        function_value_program_initialized = false;
        function_value_full_program_required = false;
        function_edge_journal.clear();
        decode_boundary_normalization_initialized = false;
        function_summary_shards.clear();
        function_resolution_shards.clear();
        function_resolution_proofs_by_site.clear();
        function_inventory_shards.clear();
        runtime_copy_result_index.clear();
        continuation_result_index.clear();
        resolution_result_index.clear();
        jump_table_result_index.clear();
        function_value_resolution_overlay_sites.clear();
        snapshot_candidate_dispatch_index.clear();
    }

    [[nodiscard]] bool prepare(
        const katana::io::ExecutableImage& image,
        const AnalysisOverrides* const next_overrides,
        const ControlFlowAnalysisOptions& next_options,
        const std::map<std::uint32_t, SeedEvidence>& next_seeds) {
        const auto next_options_binding =
            make_control_flow_session_options_binding(next_options);
        const auto immutable_ranges = image.immutable_ranges();
        const bool bounded =
            control_flow_session_seed_binding_is_bounded(next_seeds) &&
            control_flow_session_binding_is_bounded(
                image, next_overrides);
        bool seed_extension =
            root_seed_baseline.size() <= next_seeds.size();
        if (seed_extension) {
            for (const auto& [address, evidence] : root_seed_baseline) {
                const auto found = next_seeds.find(address);
                if (found == next_seeds.end() ||
                    !same_seed_evidence(evidence, found->second)) {
                    seed_extension = false;
                    break;
                }
            }
        }
        const bool immutable_binding_matches =
            image_immutable_ranges.size() == immutable_ranges.size() &&
            std::equal(image_immutable_ranges.begin(),
                       image_immutable_ranges.end(),
                       immutable_ranges.begin(), immutable_ranges.end());
        const bool root_only_reuse =
            reusable && bounded &&
            image_identity == image.analysis_instance_identity() &&
            image_revision == image.analysis_revision() &&
            image_immutable_generation == image.immutable_generation() &&
            immutable_binding_matches &&
            same_analysis_overrides(overrides, next_overrides) &&
            options == next_options_binding && seed_extension &&
            next_options.persistent_function_analysis_epoch_import_blob.empty();

        if (!root_only_reuse)
            clear_working_state(image, next_options);
        else
            guarded_native_entry_shapes->bind(image);

        reusable = false;
        candidate_binding_is_bounded = bounded;
        if (bounded)
            candidate_root_seed_baseline = next_seeds;
        else
            candidate_root_seed_baseline.clear();
        candidate_working_seed_baseline.clear();
        image_identity = image.analysis_instance_identity();
        image_revision = image.analysis_revision();
        image_immutable_generation = image.immutable_generation();
        if (bounded) {
            image_immutable_ranges.assign(immutable_ranges.begin(),
                                          immutable_ranges.end());
            overrides = next_overrides == nullptr
                            ? std::optional<AnalysisOverrides>{}
                            : std::optional<AnalysisOverrides>{*next_overrides};
        } else {
            image_immutable_ranges.clear();
            overrides.reset();
        }
        options = next_options_binding;
        function_value_full_program_required = false;
        return root_only_reuse;
    }

    void stage_terminal_seeds(
        const std::map<std::uint32_t, SeedEvidence>& seeds) {
        if (!candidate_binding_is_bounded ||
            !control_flow_session_seed_binding_is_bounded(seeds)) {
            candidate_binding_is_bounded = false;
            candidate_working_seed_baseline.clear();
            return;
        }
        candidate_working_seed_baseline = seeds;
    }

    void finish(const ControlFlowAnalysisResult& result) {
        const bool complete =
            result.termination_reason ==
                ControlFlowAnalysisTerminationReason::None &&
            !result.function_budget_exhausted;
        reusable = complete && candidate_binding_is_bounded;
        if (reusable) {
            root_seed_baseline =
                std::move(candidate_root_seed_baseline);
            working_seed_baseline =
                std::move(candidate_working_seed_baseline);
        } else {
            reset();
        }
    }

    void reset() noexcept {
        reusable = false;
        candidate_binding_is_bounded = false;
        image_identity = 0u;
        image_revision = 0u;
        image_immutable_generation = 0u;
        image_immutable_ranges.clear();
        overrides.reset();
        options = {};
        root_seed_baseline.clear();
        working_seed_baseline.clear();
        candidate_root_seed_baseline.clear();
        candidate_working_seed_baseline.clear();
        recursive_session.reset();
        recursive_index.clear();
        guarded_native_entry_shapes.reset();
        function_value_session.reset();
        jump_table_cache = JumpTableSnapshotCache{};
        cfa_scan_cache.clear();
        function_boundary_index.clear();
        function_value_program_initialized = false;
        function_value_full_program_required = false;
        function_edge_journal.clear();
        decode_boundary_normalization_initialized = false;
        function_summary_shards.clear();
        function_resolution_shards.clear();
        function_resolution_proofs_by_site.clear();
        function_inventory_shards.clear();
        runtime_copy_result_index.clear();
        continuation_result_index.clear();
        resolution_result_index.clear();
        jump_table_result_index.clear();
        function_value_resolution_overlay_sites.clear();
        snapshot_candidate_dispatch_index.clear();
    }
};

namespace {

ControlFlowAnalysisResult analyze_control_flow_session_impl(
    ControlFlowAnalysisSession::Impl& session_state,
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides,
    const ControlFlowAnalysisProgressCallback& progress_callback,
    const ControlFlowAnalysisOptions& options) {
    std::map<std::uint32_t, SeedEvidence> seeds;
    SeedLedgerTelemetry seed_telemetry;
    SeedChangeTracker pending_seed_changes;
    for (const auto entry : image.entry_points()) {
        const auto validation = validate_committed_code_address(
            image, entry);
        if (!validation.valid()) continue;
        const std::array origins{FunctionOrigin::EntryPoint};
        static_cast<void>(add_seed(
            seeds,
            validation.resolved_address,
            origins,
            true,
            ControlFlowEvidence::ProvenComplete,
            0u,
            SeedCause{SeedCauseKind::EntryPoint,
                      validation.resolved_address,
                      std::nullopt,
                      validation.resolved_address},
            &pending_seed_changes,
            &seed_telemetry));
    }
    for (const auto& symbol : image.symbols()) {
        if (symbol.kind != katana::io::SymbolKind::Function ||
            (symbol.address & 1u) != 0u)
            continue;
        const auto validation = validate_committed_code_address(
            image, symbol.address);
        if (!validation.valid()) continue;
        const std::array origins{FunctionOrigin::Symbol};
        static_cast<void>(add_seed(
            seeds,
            validation.resolved_address,
            origins,
            true,
            ControlFlowEvidence::ProvenComplete,
            0u,
            SeedCause{SeedCauseKind::Symbol,
                      validation.resolved_address,
                      std::nullopt,
                      validation.resolved_address},
            &pending_seed_changes,
            &seed_telemetry));
    }
    const bool hints = overrides != nullptr && overrides->mode == AnalysisDirectiveMode::Hint;
    std::vector<AnalysisDirectiveDiagnostic> seed_diagnostics;
    std::vector<ExactFunctionOwnershipInterval>
        exact_function_ownership;
    std::vector<std::uint32_t> non_root_function_entry_hints;
    const auto add_exact_function_ownership =
        [&](const std::uint32_t begin,
            const std::uint32_t size) {
            const auto end = static_cast<std::uint64_t>(begin) + size;
            const auto existing = std::find_if(
                exact_function_ownership.begin(),
                exact_function_ownership.end(),
                [&](const auto& interval) {
                    return interval.begin == begin;
                });
            if (existing != exact_function_ownership.end()) {
                if (existing->end != end)
                    throw std::invalid_argument(
                        "Explizite Funktionsgrenzen widersprechen sich.");
                return;
            }
            exact_function_ownership.push_back({begin, end});
        };
    if (overrides != nullptr) {
        if (hints && !overrides->function_boundaries.empty())
            override_error(
                *overrides,
                overrides->function_boundaries.front().line,
                overrides->function_boundaries.front().address,
                "function-boundary-requires-override-mode");
        for (const auto& boundary : overrides->function_boundaries) {
            if (boundary.size == 0u || (boundary.size & 1u) != 0u)
                override_error(*overrides,
                               boundary.line,
                               boundary.address,
                               "function-boundary-size-invalid");
            const auto validation = validate_committed_code_address(
                image, boundary.address, boundary.size);
            if (!validation.valid())
                override_error(*overrides,
                               boundary.line,
                               boundary.address,
                               code_address_status_name(validation.status));
            add_exact_function_ownership(
                validation.resolved_address, boundary.size);
        }
        for (const auto& entry_hint : overrides->function_entry_hints) {
            const auto validation = validate_committed_code_address(
                image, entry_hint.address, 2u);
            if (!validation.valid())
                override_error(*overrides,
                               entry_hint.line,
                               entry_hint.address,
                               code_address_status_name(validation.status));
            non_root_function_entry_hints.push_back(
                validation.resolved_address);
        }
        for (const auto& function : overrides->functions) {
            if ((function.size & 1u) != 0u) {
                if (hints) {
                    seed_diagnostics.push_back(
                        {function.line,
                         function.address,
                         AnalysisDirectiveDiagnosticStatus::Rejected,
                         "function-size-odd"});
                    continue;
                }
                override_error(*overrides,
                               function.line,
                               function.address,
                               "function-size-odd");
            }
            const auto validation = validate_committed_code_address(
                image,
                function.address,
                function.size == 0u ? 2u : function.size);
            if (!validation.valid()) {
                if (hints) {
                    seed_diagnostics.push_back({function.line,
                                                function.address,
                                                AnalysisDirectiveDiagnosticStatus::Rejected,
                                                code_address_status_name(validation.status)});
                    continue;
                }
                override_error(*overrides,
                               function.line,
                               function.address,
                               code_address_status_name(validation.status));
            }
            const std::array origins{hints ? FunctionOrigin::UserHint
                                           : FunctionOrigin::UserOverride};
            if (!hints && function.size != 0u)
                add_exact_function_ownership(
                    validation.resolved_address, function.size);
            static_cast<void>(add_seed(seeds,
                                       validation.resolved_address,
                                       origins,
                                       false,
                                       hints ? ControlFlowEvidence::HintCandidate
                                             : ControlFlowEvidence::ForcedOverride,
                                       function.size,
                                       SeedCause{
                                           SeedCauseKind::FunctionDirective,
                                           validation.resolved_address,
                                           function.line <=
                                                   std::numeric_limits<
                                                       std::uint32_t>::max()
                                               ? std::optional<std::uint32_t>{
                                                     static_cast<std::uint32_t>(
                                                         function.line)}
                                               : std::nullopt,
                                           validation.resolved_address},
                                       &pending_seed_changes,
                                       &seed_telemetry));
            if (hints) {
                seed_diagnostics.push_back({function.line,
                                            function.address,
                                            AnalysisDirectiveDiagnosticStatus::Accepted,
                                            "function-seed"});
            }
        }
    }
    std::sort(
        exact_function_ownership.begin(),
        exact_function_ownership.end(),
        [](const auto& left, const auto& right) {
            return left.begin < right.begin;
        });
    for (std::size_t index = 1u;
         index < exact_function_ownership.size(); ++index) {
        if (exact_function_ownership[index].begin <
            exact_function_ownership[index - 1u].end)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen ueberlappen sich.");
    }
    std::sort(non_root_function_entry_hints.begin(),
              non_root_function_entry_hints.end());
    non_root_function_entry_hints.erase(
        std::unique(non_root_function_entry_hints.begin(),
                    non_root_function_entry_hints.end()),
        non_root_function_entry_hints.end());

    const bool root_only_session_reuse = session_state.prepare(
        image, overrides, options, seeds);
    if (root_only_session_reuse) {
        // The ordinary seed builder starts from an empty ledger and therefore
        // labels every retained root as new. A warm session must publish only
        // the exact root extension to RecursiveAnalysisSession; removals or
        // changed contracts were already rejected by prepare().
        pending_seed_changes = {};
        for (const auto& [address, evidence] : seeds) {
            if (session_state.root_seed_baseline.contains(address)) continue;
            pending_seed_changes.changed_targets.insert(address);
            pending_seed_changes.decode_targets.insert(address);
            pending_seed_changes.metadata_targets.insert(address);
            pending_seed_changes.exact_boundary_changed =
                pending_seed_changes.exact_boundary_changed ||
                evidence.function_size != 0u;
            pending_seed_changes.facts_added += evidence.causes.size();
        }
        merge_retained_seed_evidence(
            seeds, session_state.working_seed_baseline);
    }

    ControlFlowAnalysisResult analysis;
    auto& recursive_session = *session_state.recursive_session;
    detail::RecursiveAnalysisSnapshot recursive_snapshot;
    auto& recursive_index = session_state.recursive_index;
    RecursiveAnalysisPhysicalWork recursive_seed_arena_work;
    bool recursive_result_materialized = false;
    const auto materialize_recursive_result_once = [&] {
        if (recursive_result_materialized || !recursive_snapshot.valid()) return;
        analysis.recursive = recursive_session.materialize(
            image, recursive_snapshot);
        recursive_result_materialized = true;
        ++analysis.recursive_final_materializations;
        analysis.recursive_physical_work =
            analysis.recursive.physical_work;
        analysis.recursive_physical_work.add(recursive_seed_arena_work);
    };
    const auto publish_seed_ledger = [&] {
        analysis.seed_facts.clear();
        analysis.seed_facts.reserve(seeds.size());
        for (const auto& [address, evidence] : seeds) {
            ControlFlowAnalysisResult::SeedFact fact;
            fact.target_address = address;
            if (!exact_function_strictly_contains(
                    exact_function_ownership, address))
                fact.origins.assign(evidence.origins.begin(),
                                    evidence.origins.end());
            fact.proven = evidence.proven;
            fact.evidence = evidence.evidence;
            const auto exact_size = exact_function_size_at(
                exact_function_ownership, address);
            fact.function_size = exact_size != 0u ? exact_size
                                                  : evidence.function_size;
            fact.causes = evidence.causes;
            analysis.seed_facts.push_back(std::move(fact));
        }
        analysis.seed_targets_added = seed_telemetry.targets_added;
        analysis.seed_targets_strengthened =
            seed_telemetry.targets_strengthened;
        analysis.seed_causes_added = seed_telemetry.causes_added;
        analysis.seed_decode_targets =
            seed_telemetry.decode_targets.size();
        analysis.seed_metadata_targets =
            seed_telemetry.metadata_targets.size();
    };
    GuardedCodeInventory final_guarded_code_inventory;
    GuardedCodeInventory static_callback_inventory;
    std::vector<StaticCallbackSinkContract>
        final_static_callback_sinks;
    std::vector<StaticPersistentPointerSinkContract>
        final_static_persistent_pointer_sinks;
    std::vector<StaticCallbackFieldSinkContract>
        final_static_callback_field_sinks;
    std::vector<StaticCallbackRecordTableContract>
        final_static_callback_record_tables;
    bool static_callback_contracts_materialized = false;
    auto& guarded_native_entry_shapes =
        *session_state.guarded_native_entry_shapes;
    const auto guarded_callback_candidate_is_admissible =
        [&](const std::uint32_t address) {
            return std::binary_search(
                       non_root_function_entry_hints.begin(),
                       non_root_function_entry_hints.end(), address) ||
                   guarded_native_entry_shapes.classify(address) ==
                       detail::GuardedNativeEntryShapeStatus::Valid;
        };
    auto& function_value_session =
        *session_state.function_value_session;
    static_assert(
        maximum_persistent_function_analysis_epoch_blob_bytes ==
        detail::FunctionValueAnalysisSession::
            default_maximum_persistent_epoch_blob_bytes);
    bool persistent_function_analysis_epoch_imported = false;
    bool persistent_function_analysis_epoch_dirty_after_import = false;
    std::optional<std::size_t>
        persistent_function_analysis_epoch_program_functions;
    bool persistent_function_analysis_epoch_import_terminal =
        options.persistent_function_analysis_epoch_import_blob.empty() ||
        options.persistent_function_analysis_epoch_implementation_identity
            .empty() ||
        options.maximum_persistent_function_analysis_epoch_blob_bytes == 0u ||
        options.persistent_function_analysis_epoch_import_blob.size() >
            options.maximum_persistent_function_analysis_epoch_blob_bytes;
    // ABI-less inputs still have a valid local CFG/decode contract. A caller
    // may also deliberately disable interprocedural FVA without erasing the
    // image's independently proven local call ABI. Neither case may stage an
    // interprocedural delta that FVA cannot consume.
    const bool function_value_analysis_supported =
        options.enable_function_value_analysis &&
        image.guest_call_abi() == katana::io::GuestCallAbi::SuperHC;
    auto& jump_table_cache = session_state.jump_table_cache;
    auto& cfa_scan_cache = session_state.cfa_scan_cache;
    auto& function_boundary_index = session_state.function_boundary_index;
    std::map<std::uint32_t, detail::FunctionProgramLineDelta>
        pending_function_line_deltas;
    std::map<std::uint32_t, detail::FunctionProgramBoundaryDelta>
        pending_function_boundary_deltas;
    auto& function_value_program_initialized =
        session_state.function_value_program_initialized;
    auto& function_value_full_program_required =
        session_state.function_value_full_program_required;
    auto& function_edge_journal = session_state.function_edge_journal;
    std::set<std::uint32_t> pending_function_edge_sites;
    // Edge publication may happen before decode-boundary normalization. Keep
    // the exact affected-site set alive across that publication instead of
    // rediscovering it with a full resolution/table scan in every seed round.
    std::set<std::uint32_t> pending_contract_normalization_sites;
    auto& decode_boundary_normalization_initialized =
        session_state.decode_boundary_normalization_initialized;
    auto& function_summary_shards = session_state.function_summary_shards;
    auto& function_resolution_shards =
        session_state.function_resolution_shards;
    auto& function_resolution_proofs_by_site =
        session_state.function_resolution_proofs_by_site;
    auto& function_inventory_shards =
        session_state.function_inventory_shards;
    auto& runtime_copy_result_index =
        session_state.runtime_copy_result_index;
    auto& continuation_result_index =
        session_state.continuation_result_index;
    auto& resolution_result_index =
        session_state.resolution_result_index;
    auto& jump_table_result_index =
        session_state.jump_table_result_index;
    std::set<std::pair<std::uint32_t, std::uint32_t>>
        pending_runtime_copy_seed_sources;
    std::set<std::pair<std::uint32_t, std::uint32_t>>
        pending_continuation_seed_sources;
    std::set<std::uint32_t> pending_resolution_seed_sources;
    std::set<std::uint32_t> pending_jump_table_seed_sources;
    auto& function_value_resolution_overlay_sites =
        session_state.function_value_resolution_overlay_sites;
    auto& snapshot_candidate_dispatch_index =
        session_state.snapshot_candidate_dispatch_index;
    std::set<std::uint32_t> directive_dispatches;
    std::map<std::uint32_t, std::vector<const JumpOverride*>>
        jump_overrides_by_site;
    std::map<std::uint32_t, const JumpTableOverride*>
        jump_table_overrides_by_site;
    std::set<std::uint32_t> pending_jump_override_sites;
    std::set<std::uint32_t> pending_jump_table_override_sites;
    if (overrides != nullptr) {
        for (const auto& jump : overrides->jumps) {
            directive_dispatches.insert(jump.instruction_address);
            jump_overrides_by_site[jump.instruction_address].push_back(
                &jump);
            pending_jump_override_sites.insert(
                jump.instruction_address);
        }
        for (const auto& table : overrides->jump_tables) {
            directive_dispatches.insert(table.dispatch_address);
            jump_table_overrides_by_site.emplace(
                table.dispatch_address, &table);
            pending_jump_table_override_sites.insert(
                table.dispatch_address);
        }
    }
    analysis.directive_diagnostics = seed_diagnostics;
    analysis.result_index_copy_items += seed_diagnostics.size();
    std::size_t candidate_contract_iteration = 0u;
    std::size_t round_seed_baseline = seeds.size();
    SeedChangeTracker active_round_seed_changes;
    std::size_t active_round_full_cpu_fallbacks = 0u;
    const auto mark_persistent_bypass =
        [&](const PersistentAnalysisBypassReason reason) {
            if (analysis.persistent_analysis_bypass_reason ==
                PersistentAnalysisBypassReason::None)
                analysis.persistent_analysis_bypass_reason = reason;
        };
    std::atomic_bool progress_callback_failed = false;
    const auto report_progress_detail =
        [&](const std::string_view phase,
            const FunctionValueAnalysisProgress* const function_values) {
        if (!progress_callback) return;
        ControlFlowAnalysisProgress progress;
        progress.phase = phase;
        progress.iteration = analysis.fixpoint_iterations;
        progress.seeds = seeds.size();
        progress.instructions =
            recursive_snapshot.valid()
                ? recursive_snapshot.instruction_count()
                : 0u;
        progress.contexts =
            recursive_snapshot.valid()
                ? recursive_snapshot.contextual_instruction_count()
                : 0u;
        progress.resolutions =
            resolution_result_index.size();
        progress.candidate_contract_iteration =
            candidate_contract_iteration;
        progress.candidate_contract_iteration_budget =
            maximum_function_value_candidate_contract_iterations;
        progress.round_seed_baseline = round_seed_baseline;
        progress.round_added_seeds =
            seeds.size() >= round_seed_baseline
                ? seeds.size() - round_seed_baseline
                : 0u;
        const auto combined_target_count = [](const auto& active,
                                              const auto& pending) {
            auto count = active.size();
            for (const auto address : pending)
                if (!active.contains(address)) ++count;
            return count;
        };
        progress.round_seed_facts_added =
            active_round_seed_changes.facts_added +
            pending_seed_changes.facts_added;
        progress.round_seed_targets_changed =
            combined_target_count(
                active_round_seed_changes.changed_targets,
                pending_seed_changes.changed_targets);
        progress.round_decode_targets =
            combined_target_count(
                active_round_seed_changes.decode_targets,
                pending_seed_changes.decode_targets);
        progress.round_metadata_targets =
            combined_target_count(
                active_round_seed_changes.metadata_targets,
                pending_seed_changes.metadata_targets);
        progress.round_full_cpu_fallbacks =
            active_round_full_cpu_fallbacks;
        progress.persistent_analysis_bypass_reason =
            analysis.persistent_analysis_bypass_reason;
        progress.recursive_snapshot_epochs =
            analysis.recursive_snapshot_epochs;
        progress.recursive_final_materializations =
            analysis.recursive_final_materializations;
        progress.recursive_physical_work =
            analysis.recursive_physical_work;
        progress.runtime_copy_instruction_visits =
            analysis.runtime_copy_instruction_visits;
        progress.runtime_copy_result_entries_visited =
            analysis.runtime_copy_result_entries_visited;
        progress.runtime_copy_result_entries_rebuilt =
            analysis.runtime_copy_result_entries_rebuilt;
        progress.local_control_flow_instruction_visits =
            analysis.local_control_flow_instruction_visits;
        progress.local_control_flow_result_entries_visited =
            analysis.local_control_flow_result_entries_visited;
        progress.local_control_flow_result_entries_rebuilt =
            analysis.local_control_flow_result_entries_rebuilt;
        progress.dispatch_index_entries_visited =
            analysis.dispatch_index_entries_visited;
        progress.dispatch_index_entries_rebuilt =
            analysis.dispatch_index_entries_rebuilt;
        progress.jump_table_instruction_visits =
            analysis.jump_table_instruction_visits;
        progress.jump_table_result_entries_visited =
            analysis.jump_table_result_entries_visited;
        progress.jump_table_result_entries_rebuilt =
            analysis.jump_table_result_entries_rebuilt;
        progress.function_boundary_entries_visited =
            analysis.function_boundary_entries_visited;
        progress.function_boundary_entries_rebuilt =
            analysis.function_boundary_entries_rebuilt;
        progress.function_edge_family_entries_visited =
            analysis.function_edge_family_entries_visited;
        progress.function_edge_family_entries_rebuilt =
            analysis.function_edge_family_entries_rebuilt;
        progress.function_edge_state_encode_items =
            analysis.function_edge_state_encode_items;
        progress.function_edge_state_copy_items =
            analysis.function_edge_state_copy_items;
        progress.function_edge_state_exact_compare_items =
            analysis.function_edge_state_exact_compare_items;
        progress.result_index_copy_items =
            analysis.result_index_copy_items;
        progress.result_index_sort_items =
            analysis.result_index_sort_items;
        progress.result_index_materialized_items =
            analysis.result_index_materialized_items;
        progress.growing_workset =
            progress.round_seed_targets_changed != 0u ||
            progress.round_decode_targets != 0u ||
            progress.round_metadata_targets != 0u ||
            progress.round_full_cpu_fallbacks != 0u;
        progress.function_value_active =
            function_values != nullptr;
        if (function_values != nullptr) {
            progress.function_value_subphase =
                function_values->subphase;
            progress.function_value_subphase_planned =
                function_values->subphase_planned;
            progress.function_value_subphase_processed =
                function_values->subphase_processed;
            progress.function_value_subphase_queued =
                function_values->subphase_queued;
            progress.function_value_subphase_iterations =
                function_values->subphase_iterations;
            progress.function_value_functions =
                function_values->functions;
            progress.function_value_blocks =
                function_values->blocks;
            progress.function_value_iterations =
                function_values->fixpoint_iterations;
            progress.function_value_summarized_functions =
                function_values->summarized_functions;
            progress.function_value_pending =
                function_values->pending;
            progress.function_value_active_workers =
                function_values->active_workers;
            progress.function_value_executor_running_workers =
                function_values->executor_running_workers;
            progress.function_value_executor_waiting_workers =
                function_values->executor_waiting_workers;
            progress.function_value_executor_idle_workers =
                function_values->executor_idle_workers;
            progress.function_value_executor_queued_work =
                function_values->executor_queued_work;
            progress.function_value_executor_memory_blocked_work =
                function_values->executor_memory_blocked_work;
            progress.function_value_executor_continuations =
                function_values->executor_continuations;
            progress.function_value_analysis_memory_capacity_bytes =
                function_values->analysis_memory_capacity_bytes;
            progress.function_value_analysis_memory_used_bytes =
                function_values->analysis_memory_used_bytes;
            progress.function_value_analysis_memory_peak_bytes =
                function_values->analysis_memory_peak_bytes;
            progress.function_value_logical_evaluations =
                function_values->logical_evaluations;
            progress.function_value_physical_evaluations =
                function_values->physical_evaluations;
            progress.function_value_active_evaluation_requests =
                function_values->active_evaluation_requests;
            progress.function_value_evaluation_request_nanoseconds =
                function_values->evaluation_request_nanoseconds;
            progress
                .function_value_maximum_evaluation_request_nanoseconds =
                function_values
                    ->maximum_evaluation_request_nanoseconds;
            progress.function_value_cache_key_builds =
                function_values->cache_key_builds;
            progress.function_value_active_cache_key_builds =
                function_values->active_cache_key_builds;
            progress.function_value_cache_key_build_nanoseconds =
                function_values->cache_key_build_nanoseconds;
            progress.function_value_maximum_cache_key_build_nanoseconds =
                function_values
                    ->maximum_cache_key_build_nanoseconds;
            progress.function_value_cache_waits =
                function_values->cache_waits;
            progress.function_value_active_cache_waits =
                function_values->active_cache_waits;
            progress.function_value_cache_wait_nanoseconds =
                function_values->cache_wait_nanoseconds;
            progress.function_value_maximum_cache_wait_nanoseconds =
                function_values->maximum_cache_wait_nanoseconds;
            progress.function_value_cache_replays =
                function_values->cache_replays;
            progress.function_value_active_cache_replays =
                function_values->active_cache_replays;
            progress.function_value_cache_replay_nanoseconds =
                function_values->cache_replay_nanoseconds;
            progress.function_value_maximum_cache_replay_nanoseconds =
                function_values->maximum_cache_replay_nanoseconds;
            progress.function_value_active_physical_evaluations =
                function_values->active_physical_evaluations;
            progress.function_value_physical_evaluation_nanoseconds =
                function_values->physical_evaluation_nanoseconds;
            progress
                .function_value_maximum_physical_evaluation_nanoseconds =
                function_values
                    ->maximum_physical_evaluation_nanoseconds;
            progress.function_value_cache_commits =
                function_values->cache_commits;
            progress.function_value_active_cache_commits =
                function_values->active_cache_commits;
            progress.function_value_cache_commit_nanoseconds =
                function_values->cache_commit_nanoseconds;
            progress.function_value_maximum_cache_commit_nanoseconds =
                function_values->maximum_cache_commit_nanoseconds;
            progress
                .function_value_session_cache_replay_fallback_recomputes =
                function_values->cache_replay_fallback_recomputes;
            progress
                .function_value_session_cache_diagnostic_bypass_evaluations =
                function_values->cache_diagnostic_bypass_evaluations;
            progress.function_value_multi_root_context_requests =
                function_values->multi_root_context_requests;
            progress.function_value_multi_root_unique_contexts =
                function_values->multi_root_unique_contexts;
            progress.function_value_multi_root_ready_reuses =
                function_values->multi_root_ready_reuses;
            progress.function_value_multi_root_in_flight_reuses =
                function_values->multi_root_in_flight_reuses;
            progress.function_value_multi_root_provenance_links =
                function_values->multi_root_provenance_links;
            progress.function_value_multi_root_retained_contexts =
                function_values->multi_root_retained_contexts;
            progress
                .function_value_multi_root_retained_payload_bytes =
                function_values->multi_root_retained_payload_bytes;
            progress.function_value_multi_root_evictions =
                function_values->multi_root_evictions;
            progress.function_value_resolution_functions_total =
                function_values->resolution_functions_total;
            progress.function_value_resolution_functions_started =
                function_values->resolution_functions_started;
            progress.function_value_resolution_functions_ready =
                function_values->resolution_functions_ready;
            progress.function_value_resolution_functions_committed =
                function_values->resolution_functions_committed;
            progress
                .function_value_resolution_head_of_line_index =
                function_values->resolution_head_of_line_index;
            progress
                .function_value_resolution_head_of_line_elapsed_milliseconds =
                function_values
                    ->resolution_head_of_line_elapsed_milliseconds;
            progress.function_value_configured_workers =
                function_values->configured_workers;
            progress.function_value_session_cache_lookups =
                function_values->session_cache_lookups;
            progress.function_value_session_cache_ready_hits =
                function_values->session_cache_ready_hits;
            progress
                .function_value_session_cache_in_flight_coalesces =
                function_values
                    ->session_cache_in_flight_coalesces;
            progress.function_value_session_cache_hits =
                function_values->session_cache_hits;
            progress.function_value_session_cache_misses =
                function_values->session_cache_misses;
            progress.function_value_session_cache_evictions =
                function_values->session_cache_evictions;
            progress.function_value_session_cache_entries =
                function_values->session_cache_entries;
            progress
                .function_value_session_cache_retained_payload_bytes =
                function_values
                    ->session_cache_retained_payload_bytes;
            progress.function_value_session_cache_miss_cold =
                function_values->session_cache_miss_cold;
            progress.function_value_session_cache_miss_evicted =
                function_values->session_cache_miss_evicted;
            progress
                .function_value_session_cache_miss_oversize_or_no_exact_replay =
                function_values
                    ->session_cache_miss_oversize_or_no_exact_replay;
            progress
                .function_value_session_cache_miss_function_shape_changed =
                function_values
                    ->session_cache_miss_function_shape_changed;
            progress
                .function_value_session_cache_miss_projected_ingress_changed =
                function_values
                    ->session_cache_miss_projected_ingress_changed;
            progress
                .function_value_session_cache_miss_summary_dependency_changed =
                function_values
                    ->session_cache_miss_summary_dependency_changed;
            progress
                .function_value_session_cache_miss_abi_contract_changed =
                function_values
                    ->session_cache_miss_abi_contract_changed;
            progress
                .function_value_session_cache_miss_resolution_lens_changed =
                function_values
                    ->session_cache_miss_resolution_lens_changed;
            progress
                .function_value_session_cache_miss_inventory_sink_changed =
                function_values
                    ->session_cache_miss_inventory_sink_changed;
            progress
                .function_value_session_cache_miss_isolation_partition_changed =
                function_values
                    ->session_cache_miss_isolation_partition_changed;
            progress
                .function_value_session_cache_miss_contextual_summary_changed =
                function_values
                    ->session_cache_miss_contextual_summary_changed;
            progress
                .function_value_session_cache_miss_tail_ingress_changed =
                function_values
                    ->session_cache_miss_tail_ingress_changed;
            progress.function_value_evaluation_lenses =
                function_values->evaluation_lenses;
            progress.function_value_program_graph_builds =
                function_values->program_graph_builds;
            progress.function_value_program_graph_reuses =
                function_values->program_graph_reuses;
            progress.function_value_program_graph_functions_built =
                function_values->program_graph_functions_built;
            progress.function_value_program_graph_functions_reused =
                function_values->program_graph_functions_reused;
            progress.function_value_caller_scc_invalidations =
                function_values->caller_scc_invalidations;
            progress.function_value_abi_contract_epoch_reuses =
                function_values->abi_contract_epoch_reuses;
            progress.function_value_summary_state_reuses =
                function_values->summary_state_reuses;
            progress.function_value_analysis_epochs_published =
                function_values->analysis_epochs_published;
            progress.function_value_analysis_epochs_discarded =
                function_values->analysis_epochs_discarded;
            progress.function_value_incremental_epochs_started =
                function_values->incremental_epochs_started;
            progress.function_value_resolution_root_artifacts_total =
                function_values->resolution_root_artifacts_total;
            progress.function_value_resolution_root_artifacts_reused =
                function_values->resolution_root_artifacts_reused;
            progress.function_value_resolution_root_artifacts_recomputed =
                function_values->resolution_root_artifacts_recomputed;
            progress.function_value_resolution_root_artifacts_retained =
                function_values->resolution_root_artifacts_retained;
            progress.function_value_resolution_epoch_retained_bytes =
                function_values->resolution_epoch_retained_bytes;
            progress.function_value_resolution_retention_limit_reason =
                function_values->resolution_retention_limit_reason;
            progress.function_value_dirty_sccs =
                function_values->dirty_sccs;
            progress.function_value_dirty_functions =
                function_values->dirty_functions;
            progress.function_value_dirty_inventory_sinks =
                function_values->dirty_inventory_sinks;
            progress.function_value_full_cpu_recompute_fallbacks =
                function_values->full_cpu_recompute_fallbacks;
            progress.function_value_persistent_analysis_bypass_reason =
                function_values->persistent_analysis_bypass_reason;
            progress.function_value_program_delta_entries_visited =
                function_values->program_delta_entries_visited;
            progress.function_value_function_edge_full_scans =
                function_values->function_edge_full_scans;
            progress.function_value_function_edge_full_sorts =
                function_values->function_edge_full_sorts;
            progress.function_value_candidate_call_edge_full_scans =
                function_values->candidate_call_edge_full_scans;
            progress.function_value_candidate_call_edge_full_sorts =
                function_values->candidate_call_edge_full_sorts;
            progress.function_value_candidate_tail_edge_full_scans =
                function_values->candidate_tail_edge_full_scans;
            progress.function_value_candidate_tail_edge_full_sorts =
                function_values->candidate_tail_edge_full_sorts;
            progress.function_value_graph_blocks_built =
                function_values->program_graph_blocks_built;
            progress.function_value_graph_blocks_reused =
                function_values->program_graph_blocks_reused;
            progress.function_value_graph_sccs_built =
                function_values->program_graph_sccs_built;
            progress.function_value_graph_sccs_reused =
                function_values->program_graph_sccs_reused;
            progress.function_value_resolution_dependency_nodes_built =
                function_values->resolution_dependency_nodes_built;
            progress.function_value_resolution_dependency_nodes_reused =
                function_values->resolution_dependency_nodes_reused;
            progress.function_value_resolution_dependency_sccs_built =
                function_values->resolution_dependency_sccs_built;
            progress.function_value_resolution_dependency_sccs_reused =
                function_values->resolution_dependency_sccs_reused;
            progress.function_value_abi_contract_entries_visited =
                function_values->abi_contract_entries_visited;
            progress.function_value_abi_contract_entries_rebuilt =
                function_values->abi_contract_entries_rebuilt;
            progress.function_value_summary_candidate_entries_visited =
                function_values->summary_candidate_entries_visited;
            progress.function_value_summary_candidate_entries_rebuilt =
                function_values->summary_candidate_entries_rebuilt;
            progress.function_value_inventory_topology_entries_visited =
                function_values->inventory_topology_entries_visited;
            progress
                .function_value_resolution_preparation_entries_visited =
                function_values->resolution_preparation_entries_visited;
            progress.function_value_final_materialized_blocks =
                function_values->final_materialized_blocks;
            progress.function_value_final_materialized_functions =
                function_values->final_materialized_functions;
            progress.function_value_contextual_return =
                function_values->contextual_return;
        }
        try {
            progress_callback(progress);
        } catch (...) {
            // Observation must never decide product analysis. Preserve the
            // loss so the structured telemetry terminal can fail closed.
            progress_callback_failed.store(
                true, std::memory_order_relaxed);
        }
    };
    const auto report_progress =
        [&](const std::string_view phase) {
            report_progress_detail(phase, nullptr);
    };
    const auto apply_decode_boundary_downgrades =
        [&](const std::set<std::uint32_t>* const selected_sites) {
        bool changed = false;
        const auto apply_resolution = [&](IndirectControlFlowResolution&
                                              resolution) {
            ++analysis.dispatch_index_entries_visited;
            ++analysis.decode_boundary_normalization_entries_visited;
            if (!control_flow_evidence_proven(resolution.evidence)) return;
            auto targets = resolution.targets;
            if (resolution.target.has_value())
                targets.push_back(*resolution.target);
            const bool boundaries =
                std::all_of(targets.begin(), targets.end(), [&](const auto target) {
                    return recursive_index.proven_instruction_addresses
                        .contains(target);
                });
            if (boundaries) return;
            resolution.status = ResolutionStatus::Guarded;
            resolution.evidence = ControlFlowEvidence::GuardedPartial;
            if (!resolution.reason.ends_with("-decode-candidate-only"))
                resolution.reason += "-decode-candidate-only";
            pending_function_edge_sites.insert(
                resolution.instruction_address);
            changed = true;
        };
        const auto apply_table = [&](JumpTableAnalysis& table) {
            ++analysis.jump_table_result_entries_visited;
            ++analysis.decode_boundary_normalization_entries_visited;
            if (!table.resolved ||
                !control_flow_evidence_proven(table.evidence))
                return;
            const bool boundaries =
                std::all_of(table.entries.begin(),
                            table.entries.end(),
                            [&](const auto& entry) {
                                return recursive_index
                                    .proven_instruction_addresses
                                    .contains(entry.target);
                            });
            if (boundaries) return;
            table.evidence = ControlFlowEvidence::GuardedPartial;
            pending_function_edge_sites.insert(table.dispatch_address);
            const auto resolution = resolution_result_index.find(
                table.dispatch_address);
            if (resolution != resolution_result_index.end()) {
                resolution->second.status = ResolutionStatus::Guarded;
                resolution->second.evidence =
                    ControlFlowEvidence::GuardedPartial;
                resolution->second.origin_class =
                    IndirectControlFlowOriginClass::Table;
            }
            changed = true;
        };
        if (selected_sites == nullptr) {
            for (auto& [site, resolution] : resolution_result_index) {
                static_cast<void>(site);
                apply_resolution(resolution);
            }
            for (auto& [site, table] : jump_table_result_index) {
                static_cast<void>(site);
                apply_table(table);
            }
        } else {
            for (const auto site : *selected_sites) {
                const auto resolution = resolution_result_index.find(site);
                if (resolution != resolution_result_index.end())
                    apply_resolution(resolution->second);
                const auto table = jump_table_result_index.find(site);
                if (table != jump_table_result_index.end())
                    apply_table(table->second);
            }
        }
        return changed;
    };
    const auto identity_bound_immutable_range =
        [&](const std::uint32_t address, const std::size_t size) {
            const auto* segment = image.find_segment(address, size);
            if (segment == nullptr || !segment->permissions.readable)
                return false;
            return !segment->permissions.writable ||
                   image.find_immutable_range(address, size) != nullptr;
        };
    const auto eligible_signed_relative_table_reproof =
        [&](const JumpTableAnalysis& table) {
            if (!table.resolved || table.aot_candidates_only ||
                table.encoding != JumpTableEncoding::SignedRelative16 ||
                (table.reason != "bounded-signed-relative-table" &&
                 table.reason != "identity-bound-declared-table") ||
                table.entries.empty())
                return false;
            const auto byte_count = table.entries.size() * 2u;
            return identity_bound_immutable_range(
                       table.table_address, byte_count) &&
                   identity_bound_immutable_range(
                       table.dispatch_address, 2u);
        };
    for (;;) {
        if (analysis.fixpoint_iterations >=
            options.maximum_fixpoint_iterations) {
            analysis.termination_reason =
                ControlFlowAnalysisTerminationReason::
                    AnalysisIterationBudgetExceeded;
            publish_seed_ledger();
            materialize_recursive_result_once();
            report_progress(
                "analysis-iteration-budget-exhausted");
            analysis.progress_callback_failed =
                progress_callback_failed.load(
                    std::memory_order_relaxed);
            return analysis;
        }
        active_round_seed_changes =
            std::exchange(pending_seed_changes,
                          SeedChangeTracker{});
        round_seed_baseline = seeds.size();
        active_round_full_cpu_fallbacks = 0u;
        candidate_contract_iteration = 0u;
        ++analysis.fixpoint_iterations;
        const bool has_recursive_baseline =
            recursive_snapshot.valid() || root_only_session_reuse;
        const bool recursive_full_recompute =
            has_recursive_baseline &&
            active_round_seed_changes.exact_boundary_changed;
        if (recursive_full_recompute) {
            ++analysis.recursive_full_recompute_fallbacks;
            active_round_full_cpu_fallbacks = 1u;
            mark_persistent_bypass(
                PersistentAnalysisBypassReason::FunctionBoundaryChanged);
            function_value_session
                .ensure_all_persistent_analysis_state_bypassed_once(
                    PersistentAnalysisBypassReason::FunctionBoundaryChanged);
            function_value_full_program_required = true;
        }
        report_progress("iteration-start");
        const bool recursive_cold_contract =
            !has_recursive_baseline || recursive_full_recompute;
        auto recursive_complete_seeds =
            recursive_cold_contract
                ? make_seed_vector(seeds, exact_function_ownership)
                                    : std::vector<AnalysisSeed>{};
        auto recursive_delta_seeds =
            recursive_cold_contract
                ? std::vector<AnalysisSeed>{}
                : make_seed_vector(
                      seeds,
                      exact_function_ownership,
                      &active_round_seed_changes.changed_targets);
        RecursiveAnalysisPhysicalWork seed_arena_work;
        seed_arena_work.seed_arena_copy_items =
            recursive_complete_seeds.size() + recursive_delta_seeds.size();
        seed_arena_work.seed_arena_copy_bytes =
            seed_arena_work.seed_arena_copy_items * sizeof(AnalysisSeed);
        recursive_seed_arena_work.add(seed_arena_work);
        analysis.recursive_physical_work.add(seed_arena_work);
        detail::RecursiveAnalysisDeltaJournal recursive_delta;
        recursive_delta.changed_seeds = recursive_cold_contract
                                            ? std::span<const AnalysisSeed>{}
                                            : std::span<const AnalysisSeed>{
                                                  recursive_delta_seeds};
        recursive_delta.exact_function_boundary_changed =
            recursive_full_recompute;
        recursive_delta.complete = true;
        recursive_delta.complete_seed_contract_supplied =
            recursive_cold_contract;
        const auto previous_recursive_epoch =
            recursive_snapshot.valid() ? recursive_snapshot.epoch_version()
                                       : 0u;
        auto next_recursive_snapshot = recursive_session.analyze(
            image, recursive_complete_seeds, recursive_delta,
            options.maximum_instructions, options.maximum_contexts);
        analysis.recursive_physical_work.add(
            next_recursive_snapshot.physical_work());
        bool recursive_cold_retry_performed = false;
        if (next_recursive_snapshot.cold_retry_required()) {
            recursive_cold_retry_performed = true;
            ++analysis.recursive_full_recompute_fallbacks;
            active_round_full_cpu_fallbacks = 1u;
            mark_persistent_bypass(
                PersistentAnalysisBypassReason::
                    RecursiveBaselineRejected);
            // A cold retry may follow another independently discovered reason
            // before FVA has consumed the round's strong bypass. Coalesce the
            // semantic request while retaining the first diagnostic reason.
            function_value_session
                .ensure_all_persistent_analysis_state_bypassed_once(
                    PersistentAnalysisBypassReason::
                        RecursiveBaselineRejected);
            function_value_full_program_required = true;

            recursive_complete_seeds = make_seed_vector(
                seeds, exact_function_ownership);
            RecursiveAnalysisPhysicalWork retry_seed_arena_work;
            retry_seed_arena_work.seed_arena_copy_items =
                recursive_complete_seeds.size();
            retry_seed_arena_work.seed_arena_copy_bytes =
                retry_seed_arena_work.seed_arena_copy_items *
                sizeof(AnalysisSeed);
            recursive_seed_arena_work.add(retry_seed_arena_work);
            analysis.recursive_physical_work.add(
                retry_seed_arena_work);
            detail::RecursiveAnalysisDeltaJournal cold_retry_delta;
            cold_retry_delta.complete = true;
            cold_retry_delta.complete_seed_contract_supplied = true;
            next_recursive_snapshot = recursive_session.analyze(
                image, recursive_complete_seeds, cold_retry_delta,
                options.maximum_instructions,
                options.maximum_contexts);
            analysis.recursive_physical_work.add(
                next_recursive_snapshot.physical_work());
            if (next_recursive_snapshot.cold_retry_required())
                throw std::logic_error(
                    "Recursive-Cold-Retry verlangte trotz vollstaendigem "
                    "Seedvertrag einen weiteren Retry.");
        }
        recursive_snapshot = std::move(next_recursive_snapshot);
        if (recursive_snapshot.epoch_version() != previous_recursive_epoch)
            ++analysis.recursive_snapshot_epochs;
        const bool recursive_returned_cold =
            has_recursive_baseline &&
            recursive_snapshot.baseline_status() !=
                RecursiveAnalysisBaselineStatus::Reused;
        if (recursive_returned_cold) {
            recursive_index.clear();
            cfa_scan_cache.clear();
            function_boundary_index.clear();
            pending_function_line_deltas.clear();
            pending_function_boundary_deltas.clear();
            function_edge_journal.clear();
            pending_function_edge_sites.clear();
            pending_contract_normalization_sites.clear();
            decode_boundary_normalization_initialized = false;
            function_summary_shards.clear();
            function_resolution_shards.clear();
            function_resolution_proofs_by_site.clear();
            function_inventory_shards.clear();
            runtime_copy_result_index.clear();
            continuation_result_index.clear();
            resolution_result_index.clear();
            jump_table_result_index.clear();
            pending_runtime_copy_seed_sources.clear();
            pending_continuation_seed_sources.clear();
            pending_resolution_seed_sources.clear();
            pending_jump_table_seed_sources.clear();
            function_value_resolution_overlay_sites.clear();
            snapshot_candidate_dispatch_index.clear();
        }
        recursive_index.apply(recursive_snapshot);
        // Exact non-root boundaries constrain ownership without reviving
        // unreachable metadata. Once ordinary recursive control flow has
        // actually reached such a boundary, however, it must become its own
        // function seed. Otherwise a tail branch can leave the target inside
        // the caller's IR function and a later whole-function provider cannot
        // prove the already identity-bound callee. The new seed requests a
        // cold boundary split, but it does not add any previously unreachable
        // instruction to the product closure.
        bool reached_exact_boundary_promoted = false;
        for (const auto& boundary : exact_function_ownership) {
            if (seeds.contains(boundary.begin) ||
                recursive_index.find(boundary.begin) == nullptr)
                continue;
            const auto size64 = boundary.end - boundary.begin;
            if (size64 == 0u ||
                size64 > std::numeric_limits<std::uint32_t>::max())
                throw std::logic_error(
                    "Erreichte exakte Funktionsgrenze ist nicht darstellbar.");
            const std::array origins{FunctionOrigin::UserOverride};
            reached_exact_boundary_promoted =
                add_seed(
                    seeds,
                    boundary.begin,
                    origins,
                    false,
                    ControlFlowEvidence::ForcedOverride,
                    static_cast<std::uint32_t>(size64),
                    SeedCause{SeedCauseKind::FunctionDirective,
                              boundary.begin,
                              std::nullopt,
                              boundary.begin},
                    &pending_seed_changes,
                    &seed_telemetry) ||
                reached_exact_boundary_promoted;
        }
        if (reached_exact_boundary_promoted) {
            report_progress("reached-exact-boundary-promoted");
        }
        for (const auto& line : recursive_snapshot.changed_instructions()) {
            detail::FunctionProgramLineDelta delta;
            delta.address = line.address;
            delta.value = line;
            pending_function_line_deltas.insert_or_assign(line.address,
                                                          std::move(delta));
        }
        const auto changed_function_entries =
            recursive_snapshot.changed_function_entries();
        const auto changed_functions = recursive_snapshot.changed_functions();
        for (std::size_t index = 0u;
             index < changed_function_entries.size(); ++index) {
            ++analysis.function_boundary_entries_visited;
            const auto entry = changed_function_entries[index];
            const auto& function = changed_functions[index];
            detail::FunctionProgramBoundaryDelta delta;
            delta.entry_address = entry;
            if (function.evidence == ControlFlowEvidence::Unresolved) {
                if (function_boundary_index.erase(entry) == 0u) continue;
            } else {
                const FunctionBoundary boundary{function.address,
                                                function.size};
                const auto previous = function_boundary_index.find(entry);
                if (previous != function_boundary_index.end() &&
                    previous->second.entry_address ==
                        boundary.entry_address &&
                    previous->second.size == boundary.size)
                    continue;
                function_boundary_index.insert_or_assign(entry, boundary);
                delta.value = boundary;
            }
            ++analysis.function_boundary_entries_rebuilt;
            pending_function_boundary_deltas.insert_or_assign(
                entry, std::move(delta));
        }
        cfa_scan_cache.apply(recursive_snapshot, image,
                             jump_table_cache, analysis);
        for (const auto key : cfa_scan_cache.changed_runtime_copies()) {
            ++analysis.runtime_copy_result_entries_visited;
            if (const auto* copy = cfa_scan_cache.runtime_copy(key))
                runtime_copy_result_index.insert_or_assign(key, *copy);
            else
                runtime_copy_result_index.erase(key);
            pending_runtime_copy_seed_sources.insert(key);
        }
        for (const auto key : cfa_scan_cache.changed_continuations()) {
            ++analysis.local_control_flow_result_entries_visited;
            if (const auto* continuation =
                    cfa_scan_cache.continuation(key))
                continuation_result_index.insert_or_assign(
                    key, *continuation);
            else
                continuation_result_index.erase(key);
            pending_continuation_seed_sources.insert(key);
        }
        for (const auto site : cfa_scan_cache.changed_dispatches()) {
            if (jump_overrides_by_site.contains(site))
                pending_jump_override_sites.insert(site);
            if (jump_table_overrides_by_site.contains(site))
                pending_jump_table_override_sites.insert(site);
            ++analysis.dispatch_index_entries_visited;
            if (const auto* resolution =
                    cfa_scan_cache.base_resolution(site))
                resolution_result_index.insert_or_assign(site,
                                                         *resolution);
            else
                resolution_result_index.erase(site);
            pending_resolution_seed_sources.insert(site);
            if (jump_table_result_index.erase(site) != 0u) {
                ++analysis.jump_table_result_entries_rebuilt;
                pending_jump_table_seed_sources.insert(site);
            }
            snapshot_candidate_dispatch_index.erase(site);
        }
        auto restored_function_value_resolution_overlay_sites =
            std::exchange(function_value_resolution_overlay_sites,
                          std::set<std::uint32_t>{});
        for (const auto site :
             restored_function_value_resolution_overlay_sites) {
            if (jump_overrides_by_site.contains(site))
                pending_jump_override_sites.insert(site);
            if (const auto* resolution =
                    cfa_scan_cache.base_resolution(site))
                resolution_result_index.insert_or_assign(site,
                                                         *resolution);
            else
                resolution_result_index.erase(site);
            pending_function_edge_sites.insert(site);
            pending_resolution_seed_sources.insert(site);
        }
        pending_function_edge_sites.insert(
            cfa_scan_cache.changed_dispatches().begin(),
            cfa_scan_cache.changed_dispatches().end());
        const bool attempted_recursive_incremental =
            has_recursive_baseline && !recursive_full_recompute;
        if (attempted_recursive_incremental) {
            if (recursive_snapshot.baseline_status() ==
                RecursiveAnalysisBaselineStatus::Reused) {
                ++analysis.recursive_incremental_passes;
            } else if (!recursive_cold_retry_performed) {
                ++analysis.recursive_full_recompute_fallbacks;
                active_round_full_cpu_fallbacks = 1u;
                // A rejected recursive baseline is a broken incremental
                // contract for this round. The downstream FVA epoch must not
                // reuse artifacts derived from that same stale assumption.
                mark_persistent_bypass(
                    PersistentAnalysisBypassReason::
                        RecursiveBaselineRejected);
                function_value_session
                    .ensure_all_persistent_analysis_state_bypassed_once(
                        PersistentAnalysisBypassReason::
                            RecursiveBaselineRejected);
                function_value_full_program_required = true;
            }
        }
        if (recursive_snapshot.limit() !=
            RecursiveAnalysisLimit::None) {
            const auto phase = [&]() -> std::string_view {
                switch (recursive_snapshot.limit()) {
                case RecursiveAnalysisLimit::InstructionBudgetExceeded:
                    analysis.termination_reason =
                        ControlFlowAnalysisTerminationReason::
                            InstructionBudgetExceeded;
                    return "analysis-instruction-budget-exhausted";
                case RecursiveAnalysisLimit::ContextBudgetExceeded:
                    analysis.termination_reason =
                        ControlFlowAnalysisTerminationReason::
                            AnalysisContextBudgetExceeded;
                    return "analysis-context-budget-exhausted";
                case RecursiveAnalysisLimit::None:
                    break;
                }
                return "analysis-budget-exhausted";
            }();
            publish_seed_ledger();
            materialize_recursive_result_once();
            report_progress(phase);
            analysis.progress_callback_failed =
                progress_callback_failed.load(
                    std::memory_order_relaxed);
            return analysis;
        }
        report_progress("recursive-complete");
        report_progress("local-resolution-complete");
        bool missing_override_dispatch = false;
        auto& snapshot_candidate_dispatches =
            snapshot_candidate_dispatch_index;

        for (const auto site : cfa_scan_cache.changed_dispatches()) {
            const auto resolution_entry =
                resolution_result_index.find(site);
            if (resolution_entry == resolution_result_index.end()) continue;
            auto& resolution = resolution_entry->second;
            if (!hints && directive_dispatches.contains(
                              resolution.instruction_address))
                continue;
            if (resolution.status != ResolutionStatus::Unresolved) continue;
            const auto* recognition = cfa_scan_cache.dispatch_recognition(
                resolution.instruction_address);
            if (recognition == nullptr) continue;
            if (recognition->relative_call_island.has_value()) {
                const auto& island = *recognition->relative_call_island;
                    resolution.status = ResolutionStatus::Unresolved;
                    resolution.evidence = ControlFlowEvidence::RuntimeOnly;
                    resolution.origin_class = IndirectControlFlowOriginClass::Table;
                    resolution.evidence_origins = {AnalysisEvidenceOrigin::EntrySnapshot,
                                                   AnalysisEvidenceOrigin::RuntimeClassification};
                    resolution.target.reset();
                    resolution.targets.clear();
                    resolution.analysis_candidates = island.targets;
                    std::sort(resolution.analysis_candidates.begin(),
                              resolution.analysis_candidates.end());
                    resolution.analysis_candidates.erase(
                        std::unique(resolution.analysis_candidates.begin(),
                                    resolution.analysis_candidates.end()),
                        resolution.analysis_candidates.end());
                    resolution.reason = "runtime-contract-" + island.reason;
                    snapshot_candidate_dispatches.insert(
                        resolution.instruction_address);
                    pending_resolution_seed_sources.insert(site);
                    pending_function_edge_sites.insert(site);
                    continue;
            }
            if (!recognition->jump_table.has_value()) continue;
            auto table = *recognition->jump_table;
            if (table.evidence == ControlFlowEvidence::Unresolved)
                // Preserve the authority class which established the table.
                // A natively recognized immutable table is a direct proof;
                // an externally declared table remains guarded even after
                // every target has become a proven recursive boundary. Both
                // are complete, but collapsing the latter to ProvenComplete
                // would erase its identity-bound declaration provenance.
                table.evidence =
                    table.reason == "identity-bound-declared-table"
                        ? ControlFlowEvidence::GuardedComplete
                        : ControlFlowEvidence::ProvenComplete;
            jump_table_result_index.insert_or_assign(site,
                                                     std::move(table));
            ++analysis.jump_table_result_entries_rebuilt;
            pending_jump_table_seed_sources.insert(site);
            mark_resolved_table_dispatch(resolution_result_index,
                                         jump_table_result_index.at(site));
            pending_resolution_seed_sources.insert(site);
            pending_function_edge_sites.insert(site);
        }

        if (overrides != nullptr) {
            const auto jump_override_sites =
                std::exchange(pending_jump_override_sites,
                              std::set<std::uint32_t>{});
            for (const auto site : jump_override_sites) {
                const auto directive = jump_overrides_by_site.find(site);
                if (directive == jump_overrides_by_site.end()) continue;
                const auto resolution_entry =
                    resolution_result_index.find(site);
                if (resolution_entry == resolution_result_index.end()) {
                    missing_override_dispatch = true;
                    pending_jump_override_sites.insert(site);
                    continue;
                }
                auto* resolution = &resolution_entry->second;
                for (const auto* const jump_directive :
                     directive->second) {
                    const auto& jump = *jump_directive;
                    const auto target_validation =
                        validate_committed_code_address(
                            image, jump.target);
                    if (!target_validation.valid()) {
                        if (hints) {
                            analysis.directive_diagnostics.push_back(
                                {jump.line,
                                 jump.instruction_address,
                                 AnalysisDirectiveDiagnosticStatus::Rejected,
                                 code_address_status_name(
                                     target_validation.status)});
                            continue;
                        }
                        require_override_code_address(
                            image, *overrides, jump.line, jump.target);
                    }
                    if (hints &&
                        resolution->status == ResolutionStatus::Resolved) {
                        const bool confirmed =
                            resolution->target == jump.target ||
                            std::find(resolution->targets.begin(),
                                      resolution->targets.end(),
                                      jump.target) !=
                                resolution->targets.end();
                        analysis.directive_diagnostics.push_back(
                            {jump.line,
                             jump.instruction_address,
                             confirmed
                                 ? AnalysisDirectiveDiagnosticStatus::Confirmed
                                 : AnalysisDirectiveDiagnosticStatus::Rejected,
                             confirmed ? "matches-static-proof"
                                       : "conflicts-with-static-proof"});
                        continue;
                    }
                    if (hints) {
                        if (resolution->target.has_value())
                            resolution->targets.push_back(
                                *resolution->target);
                        resolution->targets.push_back(jump.target);
                        std::sort(resolution->targets.begin(),
                                  resolution->targets.end());
                        resolution->targets.erase(
                            std::unique(resolution->targets.begin(),
                                        resolution->targets.end()),
                            resolution->targets.end());
                        resolution->target = jump.target;
                        if (control_flow_evidence_strength(
                                ControlFlowEvidence::HintCandidate) >
                            control_flow_evidence_strength(
                                resolution->evidence))
                            resolution->evidence =
                                ControlFlowEvidence::HintCandidate;
                        resolution->evidence_origins.push_back(
                            AnalysisEvidenceOrigin::UserHint);
                    } else {
                        resolution->status = ResolutionStatus::Guarded;
                        resolution->evidence =
                            ControlFlowEvidence::ForcedOverride;
                        resolution->targets.clear();
                        resolution->target = jump.target;
                        resolution->evidence_origins = {
                            AnalysisEvidenceOrigin::UserOverride};
                    }
                    std::sort(resolution->evidence_origins.begin(),
                              resolution->evidence_origins.end());
                    resolution->evidence_origins.erase(
                        std::unique(resolution->evidence_origins.begin(),
                                    resolution->evidence_origins.end()),
                        resolution->evidence_origins.end());
                    resolution->reason =
                        hints ? "user-hint" : "user-override";
                    if (hints) {
                        analysis.directive_diagnostics.push_back(
                            {jump.line,
                             jump.instruction_address,
                             AnalysisDirectiveDiagnosticStatus::Accepted,
                             "resolved-unproven-target"});
                    }
                    pending_resolution_seed_sources.insert(
                        jump.instruction_address);
                    pending_function_edge_sites.insert(
                        jump.instruction_address);
                }
            }

            const auto jump_table_override_sites =
                std::exchange(pending_jump_table_override_sites,
                              std::set<std::uint32_t>{});
            for (const auto site : jump_table_override_sites) {
                const auto directive =
                    jump_table_overrides_by_site.find(site);
                if (directive == jump_table_overrides_by_site.end())
                    continue;
                const auto& table = *directive->second;
                const auto* dispatch = recursive_index.find(
                    table.dispatch_address);
                if (dispatch == nullptr && !table.require_dispatch)
                    continue;
                const auto dispatch_validation =
                    validate_committed_code_address(image, table.dispatch_address);
                if (!dispatch_validation.valid()) {
                    if (hints) {
                        analysis.directive_diagnostics.push_back(
                            {table.line,
                             table.dispatch_address,
                             AnalysisDirectiveDiagnosticStatus::Rejected,
                             code_address_status_name(dispatch_validation.status)});
                        continue;
                    }
                    require_override_code_address(
                        image, *overrides, table.line, table.dispatch_address);
                }
                if (dispatch == nullptr) {
                    if (table.require_dispatch) {
                        missing_override_dispatch = true;
                        pending_jump_table_override_sites.insert(site);
                    }
                    continue;
                }
                if (dispatch->instruction.kind != katana::sh4::InstructionKind::Jmp &&
                    dispatch->instruction.kind != katana::sh4::InstructionKind::Jsr &&
                    dispatch->instruction.kind != katana::sh4::InstructionKind::Braf &&
                    dispatch->instruction.kind != katana::sh4::InstructionKind::Bsrf) {
                    if (hints) {
                        analysis.directive_diagnostics.push_back(
                            {table.line,
                             table.dispatch_address,
                             AnalysisDirectiveDiagnosticStatus::Rejected,
                             "dispatch-not-jmp-or-jsr"});
                        continue;
                    }
                    override_error(
                        *overrides, table.line, table.dispatch_address, "dispatch-not-jmp-or-jsr");
                }
                const bool actual_call =
                    dispatch->instruction.kind ==
                        katana::sh4::InstructionKind::Jsr ||
                    dispatch->instruction.kind ==
                        katana::sh4::InstructionKind::Bsrf;
                const bool declared_transfer_mismatch =
                    (table.transfer == JumpTableOverrideTransfer::Call &&
                     !actual_call) ||
                    (table.transfer == JumpTableOverrideTransfer::Jump &&
                     actual_call);
                if (declared_transfer_mismatch) {
                    if (hints) {
                        analysis.directive_diagnostics.push_back(
                            {table.line,
                             table.dispatch_address,
                             AnalysisDirectiveDiagnosticStatus::Rejected,
                             "declared-transfer-kind-mismatch"});
                        continue;
                    }
                    override_error(*overrides,
                                   table.line,
                                   table.dispatch_address,
                                   "declared-transfer-kind-mismatch");
                }
                const auto proven = resolution_result_index.find(
                    table.dispatch_address);
                if (hints && proven != resolution_result_index.end() &&
                    proven->second.status == ResolutionStatus::Resolved) {
                    analysis.directive_diagnostics.push_back(
                        {table.line,
                         table.dispatch_address,
                         AnalysisDirectiveDiagnosticStatus::Rejected,
                         "static-target-already-proven"});
                    continue;
                }
                const auto encoding = [&] {
                    switch (table.encoding) {
                    case JumpTableOverrideEncoding::Absolute32:
                        return JumpTableEncoding::Absolute32;
                    case JumpTableOverrideEncoding::SignedRelative16:
                        return JumpTableEncoding::SignedRelative16;
                    case JumpTableOverrideEncoding::SignedRelative32:
                        return JumpTableEncoding::SignedRelative32;
                    }
                    return JumpTableEncoding::Absolute32;
                }();
                auto jump_table =
                    encoding == JumpTableEncoding::Absolute32 &&
                            table.entry_stride == sizeof(std::uint32_t) &&
                            table.relative_base == 0u
                        ? analyze_jump_table(image,
                                             table.dispatch_address,
                                             table.table_address,
                                             table.entry_count,
                                             &jump_table_cache)
                        : analyze_declared_jump_table(image,
                                                      table.dispatch_address,
                                                      table.table_address,
                                                      table.relative_base,
                                                      table.entry_count,
                                                      table.entry_stride,
                                                      encoding);
                if (jump_table.reason == "table-segment-writable")
                    jump_table = analyze_declared_jump_table(
                        image,
                        table.dispatch_address,
                        table.table_address,
                        table.relative_base,
                        table.entry_count,
                        table.entry_stride,
                        encoding);
                jump_table.dispatch_kind =
                    actual_call
                        ? JumpTableDispatchKind::Call
                        : JumpTableDispatchKind::Jump;
                if (hints) {
                    analysis.directive_diagnostics.push_back(
                        {table.line,
                         table.dispatch_address,
                         jump_table.resolved ? AnalysisDirectiveDiagnosticStatus::Accepted
                                             : AnalysisDirectiveDiagnosticStatus::Rejected,
                         jump_table.resolved ? "jump-table-validated" : jump_table.reason});
                }
                const auto* native_recognition =
                    cfa_scan_cache.dispatch_recognition(
                        table.dispatch_address);
                const auto* native_table =
                    native_recognition != nullptr &&
                            native_recognition->jump_table.has_value()
                        ? &*native_recognition->jump_table
                        : nullptr;
                const auto* native_producer =
                    native_recognition != nullptr &&
                            native_recognition->jump_table.has_value() &&
                            native_recognition
                                ->snapshot_absolute_producer.has_value()
                        ? &*native_recognition
                               ->snapshot_absolute_producer
                        : nullptr;
                const bool native_full_table_match =
                    native_table != nullptr && native_table->resolved &&
                    !native_table->candidate_scan_truncated &&
                    native_table->dispatch_address ==
                        jump_table.dispatch_address &&
                    native_table->table_address == jump_table.table_address &&
                    native_table->target_base == jump_table.target_base &&
                    native_table->requested_entries ==
                        jump_table.requested_entries &&
                    native_table->dispatch_kind == jump_table.dispatch_kind &&
                    native_table->encoding == jump_table.encoding &&
                    native_table->entries.size() == jump_table.entries.size() &&
                    std::equal(
                        native_table->entries.begin(),
                        native_table->entries.end(),
                        jump_table.entries.begin(),
                        [](const auto& recognized, const auto& declared) {
                            return recognized.index == declared.index &&
                                   recognized.entry_address ==
                                       declared.entry_address &&
                                   recognized.target == declared.target &&
                                   recognized.accepted && declared.accepted;
                        });
                const bool native_fixed_entry_match =
                    native_table != nullptr && native_table->resolved &&
                    native_producer != nullptr &&
                    native_producer->fixed_entry &&
                    native_table->dispatch_address ==
                        jump_table.dispatch_address &&
                    native_table->dispatch_kind == jump_table.dispatch_kind &&
                    native_table->encoding == jump_table.encoding &&
                    jump_table.requested_entries == 1u &&
                    jump_table.entries.size() == 1u &&
                    jump_table.entries.front().accepted &&
                    jump_table.table_address ==
                        native_producer->fixed_entry_address &&
                    jump_table.entries.front().entry_address ==
                        native_producer->fixed_entry_address &&
                    jump_table.entries.front().target ==
                        native_producer->fixed_target &&
                    std::any_of(
                        native_table->entries.begin(),
                        native_table->entries.end(),
                        [&](const auto& recognized) {
                            return recognized.accepted &&
                                   recognized.entry_address ==
                                       native_producer->fixed_entry_address &&
                                   recognized.target ==
                                       native_producer->fixed_target;
                        });
                const bool native_entries_match =
                    native_full_table_match || native_fixed_entry_match;
                bool native_producer_identity_bound = native_entries_match;
                if (encoding == JumpTableEncoding::Absolute32) {
                    native_producer_identity_bound =
                        native_producer_identity_bound &&
                        native_producer != nullptr;
                    if (native_producer_identity_bound) {
                        const auto& producer =
                            *native_producer;
                        native_producer_identity_bound =
                            producer.literal_address != 0u &&
                            !producer.instruction_addresses.empty() &&
                            identity_bound_immutable_range(
                                producer.literal_address,
                                sizeof(std::uint32_t)) &&
                            std::all_of(
                                producer.instruction_addresses.begin(),
                                producer.instruction_addresses.end(),
                                [&](const auto address) {
                                    return identity_bound_immutable_range(
                                        address,
                                        sizeof(std::uint16_t));
                                });
                    }
                }
                jump_table.evidence =
                    hints ? ControlFlowEvidence::HintCandidate
                    : table.identity_bound_complete && jump_table.resolved &&
                              native_producer_identity_bound
                        ? ControlFlowEvidence::GuardedComplete
                        : ControlFlowEvidence::ForcedOverride;
                jump_table_result_index.insert_or_assign(
                    table.dispatch_address, std::move(jump_table));
                ++analysis.jump_table_result_entries_rebuilt;
                pending_jump_table_seed_sources.insert(
                    table.dispatch_address);
                pending_function_edge_sites.insert(
                    table.dispatch_address);
                mark_resolved_table_dispatch(
                    resolution_result_index,
                    jump_table_result_index.at(table.dispatch_address));
                pending_resolution_seed_sources.insert(
                    table.dispatch_address);
            }
        }
        const auto is_jump_table_dispatch = [&jump_table_result_index](
                                                const std::uint32_t address) {
            return jump_table_result_index.contains(address);
        };
        bool changed = false;
        for (const auto key : pending_runtime_copy_seed_sources) {
            const auto copy_entry = runtime_copy_result_index.find(key);
            if (copy_entry == runtime_copy_result_index.end()) continue;
            const auto& copy = copy_entry->second;
            ++analysis.runtime_copy_result_entries_visited;
            const std::array origins{FunctionOrigin::RuntimeCopy};
            changed = add_seed(seeds,
                               copy.source_begin,
                               origins,
                               false,
                               ControlFlowEvidence::GuardedPartial,
                               0u,
                               SeedCause{
                                   SeedCauseKind::RuntimeCodeCopySource,
                                   copy.loop_address,
                                   copy.setup_address,
                                   copy.source_begin},
                               &pending_seed_changes,
                               &seed_telemetry) ||
                      changed;
            for (const auto& candidate : copy.patch_candidates) {
                changed = add_seed(seeds,
                                   candidate.target_address,
                                   origins,
                                   false,
                                   ControlFlowEvidence::GuardedPartial,
                                   0u,
                                   SeedCause{
                                       SeedCauseKind::RuntimeCodePatch,
                                       candidate.store_instruction_address,
                                       candidate.slot_address,
                                       copy.source_begin},
                                   &pending_seed_changes,
                                   &seed_telemetry) ||
                          changed;
            }
        }
        pending_runtime_copy_seed_sources.clear();
        for (const auto key : pending_continuation_seed_sources) {
            const auto continuation_entry =
                continuation_result_index.find(key);
            if (continuation_entry == continuation_result_index.end()) continue;
            const auto& continuation = continuation_entry->second;
            ++analysis.local_control_flow_result_entries_visited;
            changed = add_seed(seeds,
                               continuation.target_address,
                               {},
                               false,
                               continuation.evidence,
                               0u,
                               SeedCause{
                                    SeedCauseKind::
                                        StaticReturnContinuation,
                                    continuation.instruction_address,
                                    continuation.register_index,
                                    std::nullopt},
                               &pending_seed_changes,
                               &seed_telemetry) ||
                      changed;
        }
        pending_continuation_seed_sources.clear();
        for (const auto site : pending_resolution_seed_sources) {
            const auto resolution_entry = resolution_result_index.find(site);
            if (resolution_entry == resolution_result_index.end()) continue;
            const auto& resolution = resolution_entry->second;
            ++analysis.dispatch_index_entries_visited;
            if (is_jump_table_dispatch(resolution.instruction_address)) continue;
            changed = add_resolution_seeds(
                          seeds,
                          resolution,
                          &pending_seed_changes,
                          &seed_telemetry) ||
                      changed;
        }
        pending_resolution_seed_sources.clear();
        for (const auto site : pending_jump_table_seed_sources) {
            const auto table_entry = jump_table_result_index.find(site);
            if (table_entry == jump_table_result_index.end()) continue;
            const auto& table = table_entry->second;
            ++analysis.jump_table_result_entries_visited;
            if (!table.resolved) continue;
            const bool is_call = table.dispatch_kind == JumpTableDispatchKind::Call;
            const bool directed = directive_dispatches.contains(table.dispatch_address);
            for (const auto& entry : table.entries) {
                if (is_call) {
                    std::vector<FunctionOrigin> origins{FunctionOrigin::JumpTableCall};
                    if (directed) {
                        origins.push_back(hints ? FunctionOrigin::UserHint
                                                : FunctionOrigin::UserOverride);
                    }
                    changed = add_seed(seeds,
                                       entry.target,
                                       origins,
                                       control_flow_evidence_proven(table.evidence),
                                       table.evidence,
                                       0u,
                                       SeedCause{
                                           SeedCauseKind::JumpTableEntry,
                                           table.dispatch_address,
                                           entry.entry_address,
                                           table.table_address},
                                       &pending_seed_changes,
                                       &seed_telemetry) ||
                              changed;
                } else {
                    changed = add_seed(seeds,
                                       entry.target,
                                       {},
                                       control_flow_evidence_proven(table.evidence),
                                       table.evidence,
                                       0u,
                                       SeedCause{
                                           SeedCauseKind::JumpTableEntry,
                                           table.dispatch_address,
                                           entry.entry_address,
                                           table.table_address},
                                       &pending_seed_changes,
                                       &seed_telemetry) ||
                              changed;
                }
            }
        }
        pending_jump_table_seed_sources.clear();

        // A table may be boundary-downgraded while its seeded case blocks are
        // still being upgraded from decode-candidate to proven instruction.
        // RecursiveAnalysisSnapshot reports that upgrade separately from its
        // instruction delta, so the incremental CFA scan does not otherwise
        // revisit the original dispatch component.  Re-open only the exact
        // immutable, non-AOT table shape produced by the bounded signed16
        // recognizer, and only after every accepted target is proven in the
        // cumulative recursive index.  No address or ownership inference is
        // performed here.
        const auto newly_proven =
            recursive_snapshot.newly_proven_instruction_addresses();
        if (!newly_proven.empty()) {
            for (auto& [site, table] : jump_table_result_index) {
                if (!eligible_signed_relative_table_reproof(table) ||
                    table.evidence != ControlFlowEvidence::GuardedPartial)
                    continue;
                const auto affected = std::any_of(
                    table.entries.begin(), table.entries.end(),
                    [&](const auto& entry) {
                        return std::binary_search(
                            newly_proven.begin(), newly_proven.end(),
                            entry.target);
                    });
                if (!affected)
                    continue;
                ++analysis.jump_table_result_entries_visited;
                ++analysis.decode_boundary_normalization_entries_visited;
                const auto boundaries = std::all_of(
                    table.entries.begin(), table.entries.end(),
                    [&](const auto& entry) {
                        return recursive_index.proven_instruction_addresses
                            .contains(entry.target);
                    });
                if (!boundaries)
                    continue;
                // Preserve the authority class that established the table.
                // Identity-bound declarations remain guarded even after the
                // recursive boundary index proves every target; only native
                // recognition may become an unconditional direct proof.
                table.evidence =
                    table.reason == "identity-bound-declared-table"
                        ? ControlFlowEvidence::GuardedComplete
                        : ControlFlowEvidence::ProvenComplete;
                ++analysis.jump_table_result_entries_rebuilt;
                mark_resolved_table_dispatch(
                    resolution_result_index, table);
                // The original boundary-downgraded seed publication carried
                // weaker evidence. Re-run the bounded table seed lane so
                // the recursive seed contract and persistent replay observe
                // the same monotone proof upgrade as the resolution/journal.
                pending_jump_table_seed_sources.insert(site);
                pending_function_edge_sites.insert(site);
                changed = true;
            }
        }
        if (changed) {
            report_progress("seed-expansion");
            continue;
        }

        // ConservativeRuntimeOnly disables the much heavier interprocedural
        // FunctionValue lattice through the analysis policy while retaining
        // the image's real SH-C ABI for exact immutable code-pointer slices.
        // It still needs statically visible callback registrations, however:
        // direct calls often forward an executable literal through one or
        // more small registrar wrappers before storing it in a callback node.
        // The bounded companion analysis below publishes only guarded AOT
        // inventory. The live runtime value remains authoritative and no
        // indirect target set is ever marked complete by this path.
        if (!function_value_analysis_supported) {
            auto callback_lines = recursive_index.materialize_lines();
            std::vector<FunctionCandidate> callback_functions;
            callback_functions.reserve(recursive_index.functions.size());
            for (const auto& [entry, function] : recursive_index.functions) {
                static_cast<void>(entry);
                callback_functions.push_back(function);
            }
            std::vector<std::uint32_t> callback_external_entries;
            callback_external_entries.reserve(seeds.size());
            for (const auto& [entry, evidence] : seeds) {
                static_cast<void>(evidence);
                if (recursive_index.find(entry) != nullptr)
                    callback_external_entries.push_back(entry);
            }
            static_callback_inventory =
                detail::analyze_static_callback_inventory(
                    image, callback_lines, callback_functions,
                    callback_external_entries,
                    non_root_function_entry_hints,
                    guarded_native_entry_shapes,
                    &final_static_callback_sinks,
                    &final_static_persistent_pointer_sinks,
                    &final_static_callback_field_sinks,
                    &final_static_callback_record_tables);
            static_callback_contracts_materialized = true;
            auto& callback_candidates =
                static_callback_inventory.stored_code_addresses;
            // The inventory already requires both semantic registrar flow
            // and either a complete standalone entry shape or an independent
            // non-root function-entry hint. Recursive CFA below remains the
            // authoritative, fail-closed materialization step.
            callback_candidates.erase(
                std::remove_if(
                    callback_candidates.begin(),
                    callback_candidates.end(),
                    [&](const auto& candidate) {
                        return !guarded_callback_candidate_is_admissible(
                            candidate.target_address);
                    }),
                callback_candidates.end());
            static_callback_inventory.candidate_count =
                callback_candidates.size();
            for (const auto& candidate :
                 callback_candidates) {
                const std::array origins{
                    FunctionOrigin::StoredCodeAddress};
                auto source_sites = candidate.store_instruction_addresses;
                std::sort(source_sites.begin(), source_sites.end());
                source_sites.erase(
                    std::unique(source_sites.begin(), source_sites.end()),
                    source_sites.end());
                auto evidence_call_sites = candidate.evidence_call_sites;
                std::sort(evidence_call_sites.begin(),
                          evidence_call_sites.end());
                evidence_call_sites.erase(
                    std::unique(evidence_call_sites.begin(),
                                evidence_call_sites.end()),
                    evidence_call_sites.end());
                auto evidence_callees = candidate.evidence_callees;
                std::sort(evidence_callees.begin(),
                          evidence_callees.end());
                evidence_callees.erase(
                    std::unique(evidence_callees.begin(),
                                evidence_callees.end()),
                    evidence_callees.end());
                const auto add_callback_seed =
                    [&](const std::optional<std::uint32_t> source_site) {
                        SeedCause cause{
                            SeedCauseKind::StoredCodeAddress,
                            source_site,
                            std::nullopt,
                            std::nullopt};
                        cause.evidence_call_sites = evidence_call_sites;
                        cause.evidence_callees = evidence_callees;
                        changed =
                            add_seed(
                                seeds,
                                candidate.target_address,
                                origins,
                                false,
                                ControlFlowEvidence::GuardedPartial,
                                0u,
                                std::move(cause),
                                &pending_seed_changes,
                                &seed_telemetry) ||
                            changed;
                    };
                if (source_sites.empty()) {
                    add_callback_seed(std::nullopt);
                } else {
                    for (const auto source_site : source_sites)
                        add_callback_seed(source_site);
                }
            }
            if (changed) {
                report_progress("static-callback-inventory-seed-expansion");
                continue;
            }
        }

        if (!persistent_function_analysis_epoch_imported &&
            !persistent_function_analysis_epoch_import_terminal &&
            persistent_function_analysis_epoch_program_functions.has_value() &&
            function_boundary_index.size() >
                *persistent_function_analysis_epoch_program_functions)
            persistent_function_analysis_epoch_import_terminal = true;
        const bool persistent_epoch_import_size_ready =
            !persistent_function_analysis_epoch_imported &&
            !persistent_function_analysis_epoch_import_terminal &&
            (!persistent_function_analysis_epoch_program_functions.has_value() ||
             function_boundary_index.size() ==
                 *persistent_function_analysis_epoch_program_functions);
        std::vector<FunctionBoundary> function_boundaries;
        if (function_value_analysis_supported &&
            (!function_value_program_initialized ||
             function_value_full_program_required ||
             persistent_epoch_import_size_ready)) {
            function_boundaries.reserve(function_boundary_index.size());
            analysis.function_boundary_entries_visited +=
                function_boundary_index.size();
            for (const auto& [entry, boundary] : function_boundary_index) {
                static_cast<void>(entry);
                function_boundaries.push_back(boundary);
            }
            analysis.function_boundary_entries_rebuilt +=
                function_boundaries.size();
        }
        const auto runtime_normalization_before =
            analysis.dispatch_index_entries_visited;
        bind_partial_runtime_contracts(
            resolution_result_index, pending_function_edge_sites,
            &pending_function_edge_sites,
            &analysis.dispatch_index_entries_visited);
        analysis.runtime_contract_normalization_entries_visited +=
            analysis.dispatch_index_entries_visited -
            runtime_normalization_before;
        pending_contract_normalization_sites.insert(
            pending_function_edge_sites.begin(),
            pending_function_edge_sites.end());
        function_edge_journal.refresh(
            pending_function_edge_sites, resolution_result_index,
            jump_table_result_index,
            analysis);
        pending_function_edge_sites.clear();
        bool boundary_contracts_active = false;
        bool function_values_stable = false;
        CandidateContractCycleLedger candidate_cycle_ledger;
        while (!function_values_stable && !changed &&
               !analysis.function_budget_exhausted) {
            candidate_contract_iteration =
                candidate_cycle_ledger.size() + 1u;
            if (candidate_cycle_ledger.already_seen(
                    boundary_contracts_active,
                    function_edge_journal, analysis)) {
                analysis.function_budget_exhausted = true;
                report_progress(
                    "function-values-candidate-contract-cycle-exhausted");
                break;
            }
            if (candidate_cycle_ledger.size() >
                maximum_function_value_candidate_contract_iterations) {
                analysis.function_budget_exhausted = true;
                report_progress(
                    "function-values-candidate-contract-budget-exhausted");
                break;
            }
            candidate_contract_iteration =
                candidate_cycle_ledger.size();
            report_progress(
                !options.enable_function_value_analysis
                    ? "function-values-skipped-by-analysis-policy"
                : !function_value_analysis_supported
                    ? "function-values-skipped-unsupported-abi"
                    : candidate_cycle_ledger.size() == 1u
                          ? "function-values-start"
                          : "function-values-candidate-contract-reconcile");
            std::optional<FunctionValueAnalysisProgress>
                latest_function_value_progress;
            FunctionValueAnalysisProgressCallback
                function_value_progress_callback;
            if (progress_callback) {
                function_value_progress_callback =
                    [&report_progress_detail,
                     &latest_function_value_progress](
                        const FunctionValueAnalysisProgress& progress) {
                        latest_function_value_progress = progress;
                        std::string phase = "function-values-";
                        phase += progress.phase;
                        phase += "-f" +
                                 std::to_string(progress.functions);
                        phase += "-b" +
                                 std::to_string(progress.blocks);
                        phase +=
                            "-k" +
                            std::to_string(progress.fixpoint_iterations);
                        phase +=
                            "-s" +
                            std::to_string(
                                progress.summarized_functions);
                        phase +=
                            "-c" +
                            std::to_string(
                                progress.resolution_functions_committed);
                        phase += "-p" +
                                 std::to_string(progress.pending);
                        phase +=
                            "-r" +
                            std::to_string(progress.resolutions);
                        report_progress_detail(
                            phase,
                            &progress);
                    };
            }
            FunctionValueAnalysisResult function_values;
            function_values.result_materialization =
                FunctionValueResultMaterialization::DeltaOnly;
            if (function_value_analysis_supported) {
                const bool persistent_epoch_imported_before_round =
                    persistent_function_analysis_epoch_imported;
                const bool persistent_epoch_import_pending =
                    !persistent_function_analysis_epoch_imported &&
                    !persistent_function_analysis_epoch_import_terminal;
                if (persistent_epoch_import_pending &&
                    persistent_function_analysis_epoch_program_functions
                        .has_value() &&
                    function_boundary_index.size() >
                        *persistent_function_analysis_epoch_program_functions)
                    persistent_function_analysis_epoch_import_terminal = true;
                const bool persistent_epoch_import_attempt_due =
                    persistent_epoch_import_pending &&
                    !persistent_function_analysis_epoch_import_terminal &&
                    (!persistent_function_analysis_epoch_program_functions
                          .has_value() ||
                     function_boundary_index.size() ==
                         *persistent_function_analysis_epoch_program_functions);
                const bool full_function_program =
                    !function_value_program_initialized ||
                    function_value_full_program_required ||
                    persistent_epoch_import_attempt_due;
                detail::FunctionProgramDelta function_program_delta;
                function_program_delta.result_materialization =
                    FunctionValueResultMaterialization::DeltaOnly;
                function_program_delta.expected_published_epoch_version =
                    function_value_session.published_epoch_version();
                function_program_delta.image_identity =
                    image.analysis_instance_identity();
                function_program_delta.image_revision =
                    image.analysis_revision();
                function_program_delta.image_immutable_generation =
                    image.immutable_generation();
                function_program_delta.kind =
                    full_function_program
                        ? detail::FunctionProgramDeltaKind::Unknown
                        : detail::FunctionProgramDeltaKind::Exact;
                if (!full_function_program) {
                    for (const auto& [address, delta] :
                         pending_function_line_deltas) {
                        static_cast<void>(address);
                        function_program_delta.changed_lines.push_back(delta);
                    }
                    for (const auto& [entry, delta] :
                         pending_function_boundary_deltas) {
                        static_cast<void>(entry);
                        function_program_delta.changed_boundaries.push_back(delta);
                    }
                    function_edge_journal.append_pending_delta(
                        function_program_delta);
                    if (function_program_delta.changed_lines.empty() &&
                        function_program_delta.changed_boundaries.empty() &&
                        function_program_delta.changed_semantic_edge_sites.empty() &&
                        function_program_delta.changed_candidate_call_sites.empty() &&
                        function_program_delta.changed_candidate_tail_sites.empty())
                        function_program_delta.kind =
                            detail::FunctionProgramDeltaKind::Unchanged;
                }
                auto function_program_lines =
                    full_function_program
                        ? recursive_index.materialize_lines()
                        : std::vector<katana::sh4::DisassemblyLine>{};
                auto function_program_edges =
                    full_function_program
                        ? function_edge_journal.materialize_all()
                        : std::vector<ResolvedControlFlowEdge>{};
                const auto function_program_boundaries =
                    full_function_program
                        ? std::span<const FunctionBoundary>(
                              function_boundaries)
                        : std::span<const FunctionBoundary>{};
                if (full_function_program) {
                    const auto materialized_program_items =
                        function_program_lines.size() +
                        function_program_edges.size();
                    analysis.result_index_copy_items +=
                        materialized_program_items;
                    analysis.result_index_materialized_items +=
                        materialized_program_items;
                }
                bool function_program_delta_staged_by_epoch_import = false;
                if (persistent_epoch_import_attempt_due) {
                    const auto import = function_value_session
                        .import_persistent_epoch_shards(
                            image,
                            function_program_lines,
                            function_boundaries,
                            function_program_edges,
                            options
                                .persistent_function_analysis_epoch_import_blob,
                            options
                                .persistent_function_analysis_epoch_implementation_identity,
                            FunctionValueResultMaterialization::DeltaOnly,
                            options
                                .maximum_persistent_function_analysis_epoch_blob_bytes);
                    using ImportStatus = detail::
                        PersistentFunctionAnalysisEpochImportStatus;
                    if (import.status == ImportStatus::Imported) {
                        persistent_function_analysis_epoch_imported = true;
                        function_program_delta_staged_by_epoch_import = true;
                        report_progress(
                            "function-values-persistent-epoch-imported");
                    } else if (import.status != ImportStatus::ProgramMismatch) {
                        persistent_function_analysis_epoch_import_terminal =
                            true;
                    } else if (import.program_functions == 0u) {
                        persistent_function_analysis_epoch_import_terminal =
                            true;
                    } else {
                        persistent_function_analysis_epoch_program_functions =
                            import.program_functions;
                        if (function_boundary_index.size() >
                            import.program_functions)
                            persistent_function_analysis_epoch_import_terminal =
                                true;
                    }
                }
                if (!function_program_delta_staged_by_epoch_import)
                    function_value_session.stage_next_function_program_delta(
                        std::move(function_program_delta));
                function_values =
                    detail::analyze_function_values_with_guarded_entry_cache(
                        image,
                        function_program_lines,
                        function_program_boundaries,
                        function_program_edges,
                        function_value_progress_callback,
                        guarded_native_entry_shapes,
                        function_value_session);
                if (persistent_epoch_imported_before_round)
                    persistent_function_analysis_epoch_dirty_after_import =
                        true;
                if (function_values.result_materialization !=
                    FunctionValueResultMaterialization::DeltaOnly)
                    throw std::logic_error(
                        "FVA-Candidate-Runde lieferte unerwartet einen "
                        "terminalen Vollsnapshot.");
                analysis
                    .function_value_inventory_topology_entries_visited +=
                    function_values.inventory_topology_entries_visited;
                analysis
                    .function_value_resolution_preparation_entries_visited +=
                    function_values
                        .resolution_preparation_entries_visited;
                function_value_program_initialized = true;
                function_value_full_program_required = false;
            }
            pending_function_line_deltas.clear();
            pending_function_boundary_deltas.clear();
            function_edge_journal.clear_pending_delta();
            if (function_values.progress_callback_failed) {
                progress_callback_failed.store(
                    true, std::memory_order_relaxed);
            }
            // The CFA scan restored these sites to their local base above.
            // Even when every owning FVA root is reused and therefore emits
            // no replacement, the still-current persistent proof shards must
            // be folded back over that base before edges and seeds are read.
            std::set<std::uint32_t> changed_resolution_shard_sites =
                std::move(
                    restored_function_value_resolution_overlay_sites);
            std::set<FunctionValueDependencyNodeId>
                changed_inventory_shard_owners;
            if (function_values.persistent_analysis_bypass_reason !=
                PersistentAnalysisBypassReason::None) {
                mark_persistent_bypass(
                    function_values.persistent_analysis_bypass_reason);
                // A bypass discovered inside FVA publishes a cold owner-shard
                // replacement set. Clear every consumer of the previous FVA
                // epoch before applying it: otherwise removed owners and
                // their resolution overlays survive a correct cold result.
                for (const auto& [site, owners] :
                     function_resolution_proofs_by_site) {
                    static_cast<void>(owners);
                    changed_resolution_shard_sites.insert(site);
                }
                changed_resolution_shard_sites.insert(
                    function_value_resolution_overlay_sites.begin(),
                    function_value_resolution_overlay_sites.end());
                for (const auto site : changed_resolution_shard_sites) {
                    if (const auto* base =
                            cfa_scan_cache.base_resolution(site))
                        resolution_result_index.insert_or_assign(site,
                                                                 *base);
                    else
                        resolution_result_index.erase(site);
                    pending_function_edge_sites.insert(site);
                    pending_resolution_seed_sources.insert(site);
                }
                for (const auto& [owner, inventory] :
                     function_inventory_shards) {
                    static_cast<void>(inventory);
                    changed_inventory_shard_owners.insert(owner);
                }
                function_summary_shards.clear();
                function_resolution_shards.clear();
                function_resolution_proofs_by_site.clear();
                function_inventory_shards.clear();
                function_value_resolution_overlay_sites.clear();
            }
            const auto erase_resolution_owner =
                [&](const FunctionValueDependencyNodeId owner) {
                    const auto previous =
                        function_resolution_shards.find(owner);
                    if (previous == function_resolution_shards.end()) return;
                    for (const auto& proof : previous->second) {
                        changed_resolution_shard_sites.insert(
                            proof.instruction_address);
                        const auto site =
                            function_resolution_proofs_by_site.find(
                                proof.instruction_address);
                        if (site ==
                            function_resolution_proofs_by_site.end())
                            continue;
                        site->second.erase(owner);
                        if (site->second.empty())
                            function_resolution_proofs_by_site.erase(site);
                    }
                    function_resolution_shards.erase(previous);
                };
            for (const auto owner :
                 function_values.removed_summary_shards)
                function_summary_shards.erase(owner);
            for (auto& replacement :
                 function_values.summary_replacements)
                function_summary_shards.insert_or_assign(
                    replacement.owner, std::move(replacement.summary));
            for (const auto owner :
                 function_values.removed_resolution_shards)
                erase_resolution_owner(owner);
            for (auto& replacement :
                 function_values.resolution_replacements) {
                const auto owner = replacement.owner;
                erase_resolution_owner(owner);
                for (const auto& proof : replacement.resolutions) {
                    changed_resolution_shard_sites.insert(
                        proof.instruction_address);
                    function_resolution_proofs_by_site
                        [proof.instruction_address]
                        .insert_or_assign(owner, proof);
                }
                function_resolution_shards.insert_or_assign(
                    owner, std::move(replacement.resolutions));
            }
            for (const auto owner :
                 function_values.removed_guarded_code_inventory_shards) {
                function_inventory_shards.erase(owner);
                changed_inventory_shard_owners.insert(owner);
            }
            for (auto& replacement :
                 function_values.guarded_code_inventory_replacements) {
                changed_inventory_shard_owners.insert(replacement.owner);
                function_inventory_shards.insert_or_assign(
                    replacement.owner, std::move(replacement.inventory));
            }
            if (function_values.budget_exhausted) {
                analysis.function_summary_iterations =
                    function_values.fixpoint_iterations;
                analysis.function_scc_count =
                    function_values.strongly_connected_components;
                analysis.unchanged_ingress_skips =
                    function_values.unchanged_ingress_skips;
                analysis.function_iteration_budget =
                    function_values.iteration_budget;
                analysis.function_budget_exhausted = true;
                // Preserve the exact fail-closed reason even on the early
                // budget exit. In particular, a local CFG fixpoint cap must
                // remain distinguishable from the outer interprocedural cap
                // in reports and product-export diagnostics.
                report_progress_detail(
                    "function-values-budget-exhausted",
                    latest_function_value_progress
                        ? &*latest_function_value_progress
                        : nullptr);
                break;
            }

            for (const auto site : changed_resolution_shard_sites) {
                const auto found = resolution_result_index.find(site);
                if (found == resolution_result_index.end()) continue;
                auto* resolution = &found->second;
                const auto* base = cfa_scan_cache.base_resolution(site);
                if (base == nullptr) continue;
                *resolution = *base;
                const auto proofs =
                    function_resolution_proofs_by_site.find(site);
                if (proofs == function_resolution_proofs_by_site.end()) {
                    pending_function_edge_sites.insert(site);
                    continue;
                }
                std::optional<InterproceduralTargetResolution> merged;
                for (const auto& [owner, proof] : proofs->second) {
                    static_cast<void>(owner);
                    if (!merged.has_value()) {
                        merged = proof;
                        continue;
                    }
                    merged->targets.insert(merged->targets.end(),
                                           proof.targets.begin(),
                                           proof.targets.end());
                    merged->call_sites.insert(merged->call_sites.end(),
                                              proof.call_sites.begin(),
                                              proof.call_sites.end());
                    merged->callees.insert(merged->callees.end(),
                                           proof.callees.begin(),
                                           proof.callees.end());
                    merged->guarded = merged->guarded || proof.guarded;
                    merged->complete = merged->complete && proof.complete;
                    if (control_flow_evidence_strength(proof.evidence) >=
                        control_flow_evidence_strength(merged->evidence)) {
                        merged->evidence = proof.evidence;
                        merged->reason = proof.reason;
                        merged->register_index = proof.register_index;
                        merged->call = proof.call;
                    }
                }
                auto& proof = *merged;
                std::sort(proof.targets.begin(), proof.targets.end());
                proof.targets.erase(
                    std::unique(proof.targets.begin(), proof.targets.end()),
                    proof.targets.end());
                std::sort(proof.call_sites.begin(), proof.call_sites.end());
                proof.call_sites.erase(
                    std::unique(proof.call_sites.begin(),
                                proof.call_sites.end()),
                    proof.call_sites.end());
                std::sort(proof.callees.begin(), proof.callees.end());
                proof.callees.erase(
                    std::unique(proof.callees.begin(), proof.callees.end()),
                    proof.callees.end());
                if (proof.targets.empty()) {
                    pending_function_edge_sites.insert(site);
                    continue;
                }
                if (base->reason ==
                        "guarded-complete-immutable-pc-relative-code-pointer-set" &&
                    base->evidence ==
                        ControlFlowEvidence::GuardedComplete) {
                    auto local_targets = base->targets;
                    if (base->target.has_value())
                        local_targets.push_back(*base->target);
                    std::sort(local_targets.begin(), local_targets.end());
                    local_targets.erase(
                        std::unique(local_targets.begin(),
                                    local_targets.end()),
                        local_targets.end());
                    const bool summary_outside_local_set = std::any_of(
                        proof.targets.begin(),
                        proof.targets.end(),
                        [&local_targets](const std::uint32_t target) {
                            return !std::binary_search(
                                local_targets.begin(),
                                local_targets.end(),
                                target);
                        });
                    const bool complete_summary_mismatch =
                        proof.complete && proof.targets != local_targets;
                    if (!summary_outside_local_set &&
                        !complete_summary_mismatch) {
                        // An incomplete function summary may observe a strict
                        // subset of the two closed local paths. It is not
                        // counterevidence. A complete summary must agree
                        // exactly, and any observed target outside the local
                        // set invalidates the promotion below.
                        continue;
                    }

                    auto candidates = base->analysis_candidates;
                    candidates.insert(candidates.end(),
                                      local_targets.begin(),
                                      local_targets.end());
                    candidates.insert(candidates.end(),
                                      proof.targets.begin(),
                                      proof.targets.end());
                    std::sort(candidates.begin(), candidates.end());
                    candidates.erase(
                        std::unique(candidates.begin(), candidates.end()),
                        candidates.end());
                    resolution->status = ResolutionStatus::Unresolved;
                    resolution->evidence = ControlFlowEvidence::RuntimeOnly;
                    resolution->evidence_origins = {
                        AnalysisEvidenceOrigin::LocalValue,
                        AnalysisEvidenceOrigin::FunctionSummary};
                    resolution->target.reset();
                    resolution->targets.clear();
                    resolution->analysis_candidates =
                        std::move(candidates);
                    resolution->reason =
                        "runtime-contract-static-code-pointer-set-summary-mismatch";
                    resolution->value_source =
                        "immutable-image-literal-set-with-summary-counterevidence";
                    resolution->definition_complete = false;
                    resolution->exact_target_guard = false;
                    resolution->exact_guard_rejection_reason =
                        ExactGuardRejectionReason::ConflictingTargets;
                    resolution->evidence_call_sites = proof.call_sites;
                    resolution->evidence_callees = proof.callees;
                    pending_function_edge_sites.insert(site);
                    pending_resolution_seed_sources.insert(site);
                    continue;
                }
                // A recognized table owns the finite AOT candidate set for
                // this dispatch. A function-summary proof may still have
                // observed one writable-snapshot value, but replacing the
                // guarded table resolution with that RuntimeOnly contract
                // would leave the table edges and site classification
                // inconsistent.
                if (is_jump_table_dispatch(proof.instruction_address) ||
                    snapshot_candidate_dispatches.contains(
                        proof.instruction_address))
                    continue;
                if (resolution->status == ResolutionStatus::Resolved ||
                    resolution->evidence ==
                        ControlFlowEvidence::ForcedOverride)
                    continue;
                if (control_flow_evidence_strength(proof.evidence) <
                    control_flow_evidence_strength(
                        resolution->evidence))
                    continue;
                const auto aggregate_guarded =
                    proof.guarded || !proof.complete;
                resolution->status =
                    aggregate_guarded ? ResolutionStatus::Guarded
                                      : ResolutionStatus::Resolved;
                resolution->evidence =
                    proof.complete
                        ? (aggregate_guarded
                               ? ControlFlowEvidence::GuardedComplete
                               : ControlFlowEvidence::ProvenComplete)
                        : ControlFlowEvidence::GuardedPartial;
                resolution->evidence_origins = {
                    AnalysisEvidenceOrigin::FunctionSummary};
                resolution->target =
                    proof.targets.size() == 1u
                        ? std::optional<std::uint32_t>(
                              proof.targets.front())
                        : std::nullopt;
                resolution->reason = std::move(proof.reason);
                resolution->targets = proof.targets;
                resolution->evidence_call_sites =
                    proof.call_sites;
                resolution->evidence_callees =
                    proof.callees;
                pending_function_edge_sites.insert(
                    resolution->instruction_address);
                pending_resolution_seed_sources.insert(
                    resolution->instruction_address);
                function_value_resolution_overlay_sites.insert(
                    resolution->instruction_address);
            }
            if (boundary_contracts_active)
                static_cast<void>(apply_decode_boundary_downgrades(
                    &changed_resolution_shard_sites));
            const auto changed_runtime_normalization_before =
                analysis.dispatch_index_entries_visited;
            bind_partial_runtime_contracts(
                resolution_result_index,
                changed_resolution_shard_sites,
                &pending_function_edge_sites,
                &analysis.dispatch_index_entries_visited);
            analysis.runtime_contract_normalization_entries_visited +=
                analysis.dispatch_index_entries_visited -
                changed_runtime_normalization_before;
            pending_resolution_seed_sources.insert(
                pending_function_edge_sites.begin(),
                pending_function_edge_sites.end());

            // Resolution targets can safely grow the outer decode graph as
            // soon as their bounded proof exists. Inventory and diagnostics,
            // however, are published only from a relationally stable pass.
            for (const auto site : pending_resolution_seed_sources) {
                const auto resolution = resolution_result_index.find(site);
                if (resolution == resolution_result_index.end()) continue;
                if (is_jump_table_dispatch(site))
                    continue;
                changed =
                    add_resolution_seeds(
                        seeds,
                        resolution->second,
                        &pending_seed_changes,
                        &seed_telemetry) ||
                    changed;
            }
            pending_resolution_seed_sources.clear();
            if (changed) break;

            // Once every proof target has had the opportunity to grow the
            // outer decode graph, fold boundary normalization into the same
            // candidate-contract snapshot as the FunctionValue
            // reconciliation.  Waiting until the unnormalized contracts had
            // first stabilized forced a whole additional FunctionValue pass
            // over an otherwise unchanged graph.
            if (!boundary_contracts_active) {
                boundary_contracts_active = true;
                pending_contract_normalization_sites.insert(
                    pending_function_edge_sites.begin(),
                    pending_function_edge_sites.end());
                if (!decode_boundary_normalization_initialized) {
                    ++analysis.decode_boundary_normalization_full_scans;
                    static_cast<void>(
                        apply_decode_boundary_downgrades(nullptr));
                    decode_boundary_normalization_initialized = true;
                } else {
                    static_cast<void>(apply_decode_boundary_downgrades(
                        &pending_contract_normalization_sites));
                }
                pending_contract_normalization_sites.insert(
                    pending_function_edge_sites.begin(),
                    pending_function_edge_sites.end());
                const auto boundary_runtime_normalization_before =
                    analysis.dispatch_index_entries_visited;
                bind_partial_runtime_contracts(
                    resolution_result_index,
                    pending_contract_normalization_sites,
                    &pending_function_edge_sites,
                    &analysis.dispatch_index_entries_visited);
                analysis.runtime_contract_normalization_entries_visited +=
                    analysis.dispatch_index_entries_visited -
                    boundary_runtime_normalization_before;
                pending_contract_normalization_sites.clear();
            }
            function_edge_journal.refresh(
                pending_function_edge_sites, resolution_result_index,
                jump_table_result_index,
                analysis);
            pending_function_edge_sites.clear();
            if (!function_edge_journal.pending_delta_empty()) continue;

            for (const auto owner : changed_inventory_shard_owners) {
                const auto inventory = function_inventory_shards.find(owner);
                if (inventory == function_inventory_shards.end()) continue;
                for (const auto& candidate :
                     inventory->second.stored_code_addresses) {
                if (!guarded_callback_candidate_is_admissible(
                        candidate.target_address))
                    continue;
                const std::array origins{
                    FunctionOrigin::StoredCodeAddress};
                auto source_sites =
                    candidate.store_instruction_addresses;
                std::sort(source_sites.begin(), source_sites.end());
                source_sites.erase(
                    std::unique(source_sites.begin(),
                                source_sites.end()),
                    source_sites.end());
                auto evidence_call_sites =
                    candidate.evidence_call_sites;
                std::sort(evidence_call_sites.begin(),
                          evidence_call_sites.end());
                evidence_call_sites.erase(
                    std::unique(evidence_call_sites.begin(),
                                evidence_call_sites.end()),
                    evidence_call_sites.end());
                auto evidence_callees = candidate.evidence_callees;
                std::sort(evidence_callees.begin(),
                          evidence_callees.end());
                evidence_callees.erase(
                    std::unique(evidence_callees.begin(),
                                evidence_callees.end()),
                    evidence_callees.end());
                const auto add_stored_seed =
                    [&](const std::optional<std::uint32_t> source_site) {
                    SeedCause cause{
                        SeedCauseKind::StoredCodeAddress,
                        source_site,
                        std::nullopt,
                        std::nullopt};
                    cause.evidence_call_sites = evidence_call_sites;
                    cause.evidence_callees = evidence_callees;
                    changed =
                        add_seed(
                            seeds,
                            candidate.target_address,
                            origins,
                            false,
                            ControlFlowEvidence::GuardedPartial,
                            0u,
                            std::move(cause),
                            &pending_seed_changes,
                            &seed_telemetry) ||
                        changed;
                };
                if (source_sites.empty()) {
                    add_stored_seed(std::nullopt);
                } else {
                    for (const auto source_site : source_sites)
                        add_stored_seed(source_site);
                }
            }
            }
            for (const auto owner : changed_inventory_shard_owners) {
                const auto inventory = function_inventory_shards.find(owner);
                if (inventory == function_inventory_shards.end()) continue;
                for (const auto& table :
                     inventory->second.returned_code_address_tables) {
                const std::array origins{
                    FunctionOrigin::GuardedSnapshot};
                auto evidence_call_sites = table.evidence_call_sites;
                std::sort(evidence_call_sites.begin(),
                          evidence_call_sites.end());
                evidence_call_sites.erase(
                    std::unique(evidence_call_sites.begin(),
                                evidence_call_sites.end()),
                    evidence_call_sites.end());
                auto evidence_callees = table.evidence_callees;
                std::sort(evidence_callees.begin(),
                          evidence_callees.end());
                evidence_callees.erase(
                    std::unique(evidence_callees.begin(),
                                evidence_callees.end()),
                    evidence_callees.end());
                for (const auto target : table.target_addresses) {
                    if (guarded_native_entry_shapes.classify(target) !=
                        detail::GuardedNativeEntryShapeStatus::Valid)
                        continue;
                    auto source_sites =
                        table.load_instruction_addresses;
                    std::sort(source_sites.begin(),
                              source_sites.end());
                    source_sites.erase(
                        std::unique(source_sites.begin(),
                                    source_sites.end()),
                        source_sites.end());
                    const auto add_returned_seed =
                        [&](const std::optional<std::uint32_t> source_site) {
                        SeedCause cause{
                            SeedCauseKind::ReturnedCodeAddressTable,
                            source_site,
                            table.table_address,
                            std::nullopt};
                        cause.evidence_call_sites =
                            evidence_call_sites;
                        cause.evidence_callees = evidence_callees;
                        changed =
                            add_seed(
                                seeds,
                                target,
                                origins,
                                false,
                                ControlFlowEvidence::GuardedPartial,
                                0u,
                                std::move(cause),
                                &pending_seed_changes,
                                 &seed_telemetry) ||
                            changed;
                    };
                    if (source_sites.empty()) {
                        add_returned_seed(std::nullopt);
                    } else {
                        for (const auto source_site : source_sites)
                            add_returned_seed(source_site);
                    }
                }
            }
            }
            if (changed) break;

            analysis.function_summary_iterations =
                function_values.fixpoint_iterations;
            analysis.function_scc_count =
                function_values.strongly_connected_components;
            analysis.unchanged_ingress_skips =
                function_values.unchanged_ingress_skips;
            analysis.function_iteration_budget =
                function_values.iteration_budget;
            analysis.function_budget_exhausted = false;
            function_values_stable = true;
            report_progress_detail(
                "function-values-complete",
                latest_function_value_progress
                    ? &*latest_function_value_progress
                    : nullptr);
        }
        // Candidate-contract iterations own only the reconciliation epoch
        // above.  Summary expansion and the later TerminalFull projection
        // have independent, epoch-local progress counters and must not be
        // folded into the final candidate scope.
        candidate_contract_iteration = 0u;
        if (analysis.function_budget_exhausted) {
            // Product export rejects both an internal summary-budget loss and
            // a candidate-contract closure that cannot reach a fixed point.
            break;
        }
        report_progress("summary-seed-expansion");
        if (!changed && missing_override_dispatch && overrides != nullptr) {
            for (const auto& jump : overrides->jumps) {
                if (!resolution_result_index.contains(
                        jump.instruction_address)) {
                    if (hints) {
                        analysis.directive_diagnostics.push_back(
                            {jump.line,
                             jump.instruction_address,
                             AnalysisDirectiveDiagnosticStatus::Stale,
                             "dispatch-not-discovered-indirect-control-flow"});
                        continue;
                    }
                    override_error(*overrides,
                                   jump.line,
                                   jump.instruction_address,
                                   "dispatch-not-discovered-indirect-control-flow");
                }
            }
            for (const auto& table : overrides->jump_tables) {
                if (!table.require_dispatch) continue;
                if (recursive_index.find(table.dispatch_address) == nullptr) {
                    if (hints) {
                        analysis.directive_diagnostics.push_back(
                            {table.line,
                             table.dispatch_address,
                             AnalysisDirectiveDiagnosticStatus::Stale,
                             "dispatch-not-discovered"});
                        continue;
                    }
                    override_error(
                        *overrides, table.line, table.dispatch_address, "dispatch-not-discovered");
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    ++analysis.decode_boundary_normalization_full_scans;
    const auto final_boundary_change =
        apply_decode_boundary_downgrades(nullptr);
    ++analysis.runtime_contract_normalization_full_scans;
    const auto terminal_runtime_normalization_before =
        analysis.dispatch_index_entries_visited;
    bind_partial_runtime_contracts(
        resolution_result_index, &pending_function_edge_sites,
        &analysis.dispatch_index_entries_visited);
    analysis.runtime_contract_normalization_entries_visited +=
        analysis.dispatch_index_entries_visited -
        terminal_runtime_normalization_before;
    function_edge_journal.refresh(
        pending_function_edge_sites, resolution_result_index,
        jump_table_result_index, analysis);
    pending_function_edge_sites.clear();
    if (!analysis.function_budget_exhausted &&
        (final_boundary_change ||
         !function_edge_journal.pending_delta_empty())) {
        // The stable candidate-contract loop must already contain terminal
        // boundary/runtime normalization. Keep the terminal FVA snapshot
        // pinned for diagnostics, but reject product completeness instead
        // of publishing inventory derived from stale edges.
        analysis.function_budget_exhausted = true;
        report_progress("function-values-boundary-contract-stale");
    }
    report_progress("analysis-terminal-materialization-start");
    if (function_value_analysis_supported &&
        function_value_program_initialized) {
        detail::FunctionProgramDelta terminal_delta;
        terminal_delta.kind = detail::FunctionProgramDeltaKind::Unchanged;
        terminal_delta.result_materialization =
            FunctionValueResultMaterialization::TerminalFull;
        terminal_delta.expected_published_epoch_version =
            function_value_session.published_epoch_version();
        terminal_delta.image_identity = image.analysis_instance_identity();
        terminal_delta.image_revision = image.analysis_revision();
        terminal_delta.image_immutable_generation =
            image.immutable_generation();
        function_value_session.stage_next_function_program_delta(
            std::move(terminal_delta));
        FunctionValueAnalysisProgressCallback terminal_progress_callback;
        if (progress_callback) {
            terminal_progress_callback =
                [&](const FunctionValueAnalysisProgress& progress) {
                    if (progress.phase == "terminal-materialized")
                        report_progress_detail(
                            "function-values-terminal-materialized",
                            &progress);
                };
        }
        const std::vector<katana::sh4::DisassemblyLine> no_lines;
        const std::vector<FunctionBoundary> no_boundaries;
        const std::vector<ResolvedControlFlowEdge> no_edges;
        auto terminal_function_values =
            detail::analyze_function_values_with_guarded_entry_cache(
                image, no_lines, no_boundaries, no_edges,
                terminal_progress_callback, guarded_native_entry_shapes,
                function_value_session);
        if (terminal_function_values.result_materialization !=
            FunctionValueResultMaterialization::TerminalFull)
            throw std::logic_error(
                "FVA-Terminalaufruf lieferte keinen Vollsnapshot.");
        if (terminal_function_values.progress_callback_failed)
            progress_callback_failed.store(true,
                                           std::memory_order_relaxed);
        if (terminal_function_values.budget_exhausted) {
            analysis.function_budget_exhausted = true;
            report_progress("function-values-terminal-budget-exhausted");
        }
        if (terminal_function_values.persistent_analysis_bypass_reason !=
            PersistentAnalysisBypassReason::None) {
            mark_persistent_bypass(
                terminal_function_values
                    .persistent_analysis_bypass_reason);
            // A terminal cold bypass has no subsequent candidate-contract
            // round in which CFA can reconcile replacement proofs back into
            // its resolution/edge consumers. Preserve diagnostics but reject
            // product completeness rather than publishing mixed epochs.
            analysis.function_budget_exhausted = true;
        }
        analysis.function_summary_iterations =
            terminal_function_values.fixpoint_iterations;
        analysis.function_scc_count =
            terminal_function_values.strongly_connected_components;
        analysis.unchanged_ingress_skips =
            terminal_function_values.unchanged_ingress_skips;
        analysis.function_iteration_budget =
            terminal_function_values.iteration_budget;
        analysis.function_value_summaries =
            std::move(terminal_function_values.summaries);
        auto guarded_code_inventory =
            std::move(terminal_function_values.guarded_code_inventory);
        analysis.raw_stored_code_inventory_candidates =
            guarded_code_inventory.raw_stored_candidate_count;
        analysis.raw_stored_code_inventory_budget =
            guarded_code_inventory.raw_stored_candidate_budget;
        analysis.raw_stored_code_inventory_truncated =
            guarded_code_inventory.raw_stored_candidates_truncated;
        analysis.guarded_code_inventory_candidates =
            guarded_code_inventory.candidate_count;
        analysis.guarded_code_inventory_budget =
            guarded_code_inventory.candidate_budget;
        analysis.guarded_code_inventory_candidate_budget_exhausted =
            guarded_code_inventory.candidate_budget_exhausted;
        analysis.guarded_code_inventory_walk =
            guarded_code_inventory.walk_diagnostics;
        analysis.guarded_code_shape_validation_work =
            guarded_code_inventory.shape_validation_work;
        analysis.guarded_code_shape_validation_work_budget =
            guarded_code_inventory.shape_validation_work_budget;
        analysis.guarded_code_shape_budget_exceeded_candidates =
            guarded_code_inventory.shape_budget_exceeded_candidates;
        analysis.candidate_inventory_truncated =
            guarded_code_inventory.candidate_inventory_truncated;
        analysis.returned_table_scan_truncated =
            guarded_code_inventory.table_scan_truncated;
        final_guarded_code_inventory = std::move(guarded_code_inventory);
    } else if (!function_value_analysis_supported) {
        analysis.raw_stored_code_inventory_candidates =
            static_callback_inventory.raw_stored_candidate_count;
        analysis.raw_stored_code_inventory_budget =
            static_callback_inventory.raw_stored_candidate_budget;
        analysis.raw_stored_code_inventory_truncated =
            static_callback_inventory.raw_stored_candidates_truncated;
        analysis.guarded_code_inventory_candidates =
            static_callback_inventory.candidate_count;
        analysis.guarded_code_inventory_budget =
            static_callback_inventory.candidate_budget;
        analysis.guarded_code_inventory_candidate_budget_exhausted =
            static_callback_inventory.candidate_budget_exhausted;
        analysis.guarded_code_inventory_walk =
            static_callback_inventory.walk_diagnostics;
        analysis.candidate_inventory_truncated =
            static_callback_inventory.candidate_inventory_truncated;
        const auto& shape_statistics =
            guarded_native_entry_shapes.statistics();
        analysis.guarded_code_shape_validation_work =
            shape_statistics.work;
        analysis.guarded_code_shape_validation_work_budget =
            shape_statistics.work_budget;
        analysis.guarded_code_shape_budget_exceeded_candidates =
            shape_statistics.shape_budget_exceeded;
        final_guarded_code_inventory =
            std::move(static_callback_inventory);
    }
    analysis.static_callback_sinks =
        std::move(final_static_callback_sinks);
    analysis.static_persistent_pointer_sinks =
        std::move(final_static_persistent_pointer_sinks);
    analysis.static_callback_field_sinks =
        std::move(final_static_callback_field_sinks);
    analysis.static_callback_record_tables =
        std::move(final_static_callback_record_tables);
    analysis.static_callback_contracts_materialized =
        static_callback_contracts_materialized;
    materialize_recursive_result_once();
    analysis.runtime_code_copies.copies.reserve(
        runtime_copy_result_index.size());
    for (const auto& [key, copy] : runtime_copy_result_index) {
        static_cast<void>(key);
        analysis.runtime_code_copies.copies.push_back(copy);
    }
    analysis.static_return_continuations.reserve(
        continuation_result_index.size());
    for (const auto& [key, continuation] : continuation_result_index) {
        static_cast<void>(key);
        analysis.static_return_continuations.push_back(continuation);
    }
    analysis.indirect_control_flow.reserve(resolution_result_index.size());
    for (const auto& [site, resolution] : resolution_result_index) {
        static_cast<void>(site);
        analysis.indirect_control_flow.push_back(resolution);
    }
    analysis.jump_tables.reserve(jump_table_result_index.size());
    for (const auto& [site, table] : jump_table_result_index) {
        static_cast<void>(site);
        analysis.jump_tables.push_back(table);
    }
    const auto terminal_result_index_items =
        analysis.runtime_code_copies.copies.size() +
        analysis.static_return_continuations.size() +
        analysis.indirect_control_flow.size() + analysis.jump_tables.size();
    analysis.result_index_copy_items += terminal_result_index_items;
    analysis.result_index_materialized_items += terminal_result_index_items;
    report_progress("fixpoint-complete");
    auto guarded_aot_entries = collect_guarded_aot_entries(
        image, analysis, final_guarded_code_inventory);
    analysis.guarded_aot_entries =
        std::move(guarded_aot_entries.entries);
    analysis.guarded_aot_entry_rejections =
        std::move(guarded_aot_entries.rejections);
    analysis.resolved_edges =
        collect_resolved_edges(analysis.indirect_control_flow, analysis.jump_tables);
    analysis.sites.reserve(analysis.indirect_control_flow.size());
    for (const auto& resolution : analysis.indirect_control_flow) {
        ControlFlowSite site;
        site.instruction_address = resolution.instruction_address;
        site.kind = resolution.kind;
        site.evidence = resolution.evidence;
        site.origin_class = resolution.origin_class;
        site.evidence_origins = resolution.evidence_origins;
        site.targets = resolution.targets;
        if (resolution.target.has_value()) site.targets.push_back(*resolution.target);
        analysis.result_index_sort_items += site.targets.size();
        std::sort(site.targets.begin(), site.targets.end());
        site.targets.erase(std::unique(site.targets.begin(), site.targets.end()),
                           site.targets.end());
        site.evidence_call_sites = resolution.evidence_call_sites;
        site.evidence_callees = resolution.evidence_callees;
        analysis.sites.push_back(std::move(site));
    }
    analysis.result_index_sort_items += analysis.sites.size();
    std::sort(
        analysis.sites.begin(), analysis.sites.end(), [](const auto& left, const auto& right) {
            return left.instruction_address < right.instruction_address;
        });
    analysis.result_index_sort_items +=
        analysis.directive_diagnostics.size();
    std::sort(analysis.directive_diagnostics.begin(),
              analysis.directive_diagnostics.end(),
              [](const auto& left, const auto& right) {
                  if (left.address != right.address) return left.address < right.address;
                  if (left.line != right.line) return left.line < right.line;
                  return left.status < right.status;
              });
    std::set<std::uint32_t> symbolic_candidates;
    for (const auto& function : analysis.recursive.functions) {
        symbolic_candidates.insert(function.address);
    }
    for (const auto& entry : analysis.guarded_aot_entries) {
        symbolic_candidates.insert(entry.guest_address);
        symbolic_candidates.insert(entry.shared_body_address);
        symbolic_candidates.insert(
            entry.source_sites.begin(), entry.source_sites.end());
        symbolic_candidates.insert(
            entry.source_objects.begin(), entry.source_objects.end());
    }
    for (const auto& rejection :
         analysis.guarded_aot_entry_rejections) {
        symbolic_candidates.insert(rejection.guest_address);
        symbolic_candidates.insert(rejection.resolved_address);
        symbolic_candidates.insert(rejection.source_sites.begin(),
                                   rejection.source_sites.end());
        symbolic_candidates.insert(rejection.source_objects.begin(),
                                   rejection.source_objects.end());
    }
    for (const auto& resolution : analysis.indirect_control_flow) {
        symbolic_candidates.insert(resolution.instruction_address);
        if (resolution.target.has_value()) symbolic_candidates.insert(*resolution.target);
        symbolic_candidates.insert(resolution.targets.begin(), resolution.targets.end());
        symbolic_candidates.insert(resolution.analysis_candidates.begin(),
                                   resolution.analysis_candidates.end());
        symbolic_candidates.insert(resolution.evidence_call_sites.begin(),
                                   resolution.evidence_call_sites.end());
        symbolic_candidates.insert(resolution.evidence_callees.begin(),
                                   resolution.evidence_callees.end());
    }
    for (const auto& continuation : analysis.static_return_continuations) {
        symbolic_candidates.insert(continuation.instruction_address);
        symbolic_candidates.insert(continuation.target_address);
    }
    for (const auto& table : analysis.jump_tables) {
        symbolic_candidates.insert(table.dispatch_address);
        symbolic_candidates.insert(table.table_address);
        for (const auto& entry : table.entries)
            symbolic_candidates.insert(entry.target);
    }
    for (const auto& copy : analysis.runtime_code_copies.copies) {
        symbolic_candidates.insert(copy.setup_address);
        symbolic_candidates.insert(copy.loop_address);
        symbolic_candidates.insert(copy.source_begin);
        symbolic_candidates.insert(copy.source_end_inclusive);
        for (const auto& candidate : copy.patch_candidates) {
            symbolic_candidates.insert(candidate.store_instruction_address);
            symbolic_candidates.insert(candidate.slot_address);
            symbolic_candidates.insert(candidate.target_address);
        }
    }
    for (const auto& diagnostic : analysis.recursive.diagnostics) {
        symbolic_candidates.insert(diagnostic.address);
    }
    for (const auto& diagnostic : analysis.directive_diagnostics) {
        symbolic_candidates.insert(diagnostic.address);
    }
    const SymbolNameIndex symbol_index(image);
    for (const auto candidate : symbolic_candidates) {
        if (auto symbol = symbol_index.resolve(candidate); symbol.has_value()) {
            analysis.symbolic_addresses.push_back(std::move(*symbol));
        }
    }
    std::vector<FunctionBoundary> final_function_boundaries;
    final_function_boundaries.reserve(
        analysis.recursive.functions.size());
    for (const auto& function : analysis.recursive.functions) {
        final_function_boundaries.push_back(
            {function.address, function.size});
    }
    std::vector<std::uint32_t> final_block_leaders;
    final_block_leaders.reserve(
        final_function_boundaries.size() * 2u +
        analysis.guarded_aot_entries.size());
    for (const auto& boundary : final_function_boundaries) {
        final_block_leaders.push_back(boundary.entry_address);
        if (boundary.size != 0u) {
            const auto end =
                static_cast<std::uint64_t>(boundary.entry_address) +
                boundary.size;
            if (end <= std::numeric_limits<std::uint32_t>::max())
                final_block_leaders.push_back(
                    static_cast<std::uint32_t>(end));
        }
    }
    for (const auto& entry : analysis.guarded_aot_entries)
        final_block_leaders.push_back(entry.guest_address);
    std::vector<std::uint32_t> final_normal_entry_leaders;
    // Image entry points are authoritative normal-entry contexts even when
    // the same physical halfword is also the delay slot of a preceding
    // branch. Seed facts retain EntryPoint provenance, so the origin-empty
    // filter below cannot be used as a proxy for this contract.
    for (const auto entry : image.entry_points()) {
        const auto validation = validate_committed_code_address(image, entry);
        if (validation.valid())
            final_normal_entry_leaders.push_back(validation.resolved_address);
    }
    for (const auto& seed : analysis.seed_facts) {
        if (seed.origins.empty())
            final_normal_entry_leaders.push_back(seed.target_address);
    }
    std::sort(final_normal_entry_leaders.begin(),
              final_normal_entry_leaders.end());
    final_normal_entry_leaders.erase(
        std::unique(final_normal_entry_leaders.begin(),
                    final_normal_entry_leaders.end()),
        final_normal_entry_leaders.end());
    const auto final_blocks = build_basic_blocks(
        analysis.recursive.instructions,
        analysis.resolved_edges,
        final_block_leaders,
        final_normal_entry_leaders);
    analysis.instruction_arena =
        std::make_shared<const InstructionArena>(analysis.recursive.instructions);
    analysis.block_spans = build_block_spans(*analysis.instruction_arena, final_blocks);
    std::vector<std::string> evidence_strings;
    for (const auto& diagnostic : analysis.recursive.diagnostics)
        evidence_strings.push_back(diagnostic.reason);
    for (const auto& diagnostic : analysis.directive_diagnostics)
        evidence_strings.push_back(diagnostic.reason);
    for (const auto& resolution : analysis.indirect_control_flow)
        evidence_strings.push_back(resolution.reason);
    for (const auto& continuation : analysis.static_return_continuations)
        evidence_strings.push_back(continuation.reason);
    for (const auto& rejection :
         analysis.guarded_aot_entry_rejections)
        evidence_strings.push_back(
            guarded_aot_entry_rejection_reason_name(rejection.reason));
    for (const auto& table : analysis.jump_tables) {
        evidence_strings.push_back(table.reason);
        for (const auto& entry : table.entries)
            evidence_strings.push_back(entry.reason);
    }
    for (const auto& copy : analysis.runtime_code_copies.copies)
        evidence_strings.push_back(copy.reason);
    analysis.result_index_sort_items += evidence_strings.size();
    std::sort(evidence_strings.begin(), evidence_strings.end());
    evidence_strings.erase(std::unique(evidence_strings.begin(), evidence_strings.end()),
                           evidence_strings.end());
    for (const auto& evidence : evidence_strings)
        static_cast<void>(analysis.evidence_ids.intern(evidence));
    publish_seed_ledger();
    analysis.jump_table_cache = jump_table_cache.counters();
    report_progress("analysis-terminal-materialized");
    analysis.progress_callback_failed =
        progress_callback_failed.load(
            std::memory_order_relaxed);
    if (analysis.termination_reason ==
            ControlFlowAnalysisTerminationReason::None &&
        !analysis.function_budget_exhausted &&
        function_value_analysis_supported &&
        function_value_program_initialized &&
        options.persistent_function_analysis_epoch_publish_callback &&
        !options.persistent_function_analysis_epoch_implementation_identity
             .empty() &&
        options.maximum_persistent_function_analysis_epoch_blob_bytes != 0u &&
        (!persistent_function_analysis_epoch_imported ||
         persistent_function_analysis_epoch_dirty_after_import)) {
        try {
            report_progress(
                "function-values-persistent-epoch-export-start");
            const auto blob = function_value_session
                .export_persistent_epoch_shards(
                    image,
                    options
                        .persistent_function_analysis_epoch_implementation_identity,
                    options
                        .maximum_persistent_function_analysis_epoch_blob_bytes);
            if (!blob.empty())
                options.persistent_function_analysis_epoch_publish_callback(
                    blob);
            if (!blob.empty())
                report_progress(
                    "function-values-persistent-epoch-published");
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            // Non-resource cache publication failures cannot invalidate the
            // authoritative fresh result.
        }
    }
    session_state.stage_terminal_seeds(seeds);
    report_progress("complete");
    return analysis;
}

} // namespace

ControlFlowAnalysisSession::ControlFlowAnalysisSession()
    : impl_(std::make_unique<Impl>()) {}

ControlFlowAnalysisSession::~ControlFlowAnalysisSession() = default;
ControlFlowAnalysisSession::ControlFlowAnalysisSession(
    ControlFlowAnalysisSession&&) noexcept = default;
ControlFlowAnalysisSession& ControlFlowAnalysisSession::operator=(
    ControlFlowAnalysisSession&&) noexcept = default;

ControlFlowAnalysisResult ControlFlowAnalysisSession::analyze(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* const overrides,
    const ControlFlowAnalysisProgressCallback& progress_callback,
    const ControlFlowAnalysisOptions& options) {
    if (impl_ == nullptr)
        throw std::logic_error(
            "Verschobene ControlFlowAnalysisSession kann nicht analysieren.");
    if (impl_->active)
        throw std::logic_error(
            "ControlFlowAnalysisSession darf nicht gleichzeitig analysieren.");
    impl_->active = true;
    try {
        auto result = analyze_control_flow_session_impl(
            *impl_, image, overrides, progress_callback, options);
        impl_->finish(result);
        impl_->active = false;
        return result;
    } catch (...) {
        impl_->reset();
        impl_->active = false;
        throw;
    }
}

void ControlFlowAnalysisSession::reset() noexcept {
    if (impl_ == nullptr || impl_->active) return;
    impl_->reset();
}

ControlFlowAnalysisResult analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* const overrides,
    const ControlFlowAnalysisProgressCallback& progress_callback,
    const ControlFlowAnalysisOptions& options) {
    ControlFlowAnalysisSession session;
    return session.analyze(
        image, overrides, progress_callback, options);
}

ControlFlowAnalysisResult analyze_control_flow(const katana::io::ExecutableImage& image,
                                               const AnalysisOverrides* overrides) {
    return analyze_control_flow(image, overrides, {}, false);
}

ControlFlowAnalysisResult analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides,
    const ControlFlowAnalysisProgressCallback& progress_callback) {
    return analyze_control_flow(
        image, overrides, progress_callback, false);
}

ControlFlowAnalysisResult analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides,
    const ControlFlowAnalysisProgressCallback& progress_callback,
    const bool detailed_cache_miss_telemetry) {
    ControlFlowAnalysisOptions options;
    options.detailed_cache_miss_telemetry =
        detailed_cache_miss_telemetry;
    return analyze_control_flow(
        image, overrides, progress_callback, options);
}

const char*
analysis_directive_diagnostic_status_name(const AnalysisDirectiveDiagnosticStatus status) noexcept {
    switch (status) {
    case AnalysisDirectiveDiagnosticStatus::Accepted:
        return "accepted";
    case AnalysisDirectiveDiagnosticStatus::Confirmed:
        return "confirmed";
    case AnalysisDirectiveDiagnosticStatus::Rejected:
        return "rejected";
    case AnalysisDirectiveDiagnosticStatus::Stale:
        return "stale";
    }
    return "unknown";
}

const char*
guarded_aot_entry_origin_name(const GuardedAotEntryOrigin origin) noexcept {
    switch (origin) {
    case GuardedAotEntryOrigin::IndirectCall:
        return "indirect-call";
    case GuardedAotEntryOrigin::TailIngress:
        return "tail-ingress";
    case GuardedAotEntryOrigin::JumpTableTail:
        return "jump-table-tail";
    case GuardedAotEntryOrigin::StaticReturn:
        return "static-return";
    case GuardedAotEntryOrigin::StoredCodeAddress:
        return "stored-code-address";
    case GuardedAotEntryOrigin::ReturnedCodeAddressTable:
        return "returned-code-address-table";
    }
    return "unknown";
}

const char*
guarded_aot_entry_rejection_reason_name(
    const GuardedAotEntryRejectionReason reason) noexcept {
    switch (reason) {
    case GuardedAotEntryRejectionReason::OddAddress:
        return "odd-address";
    case GuardedAotEntryRejectionReason::OutsideSegments:
        return "outside-segments";
    case GuardedAotEntryRejectionReason::NotCodeSegment:
        return "not-code-segment";
    case GuardedAotEntryRejectionReason::NotExecutableSegment:
        return "not-executable-segment";
    case GuardedAotEntryRejectionReason::OutsideCommittedData:
        return "outside-committed-data";
    case GuardedAotEntryRejectionReason::InstructionNotAnalyzed:
        return "instruction-not-analyzed";
    case GuardedAotEntryRejectionReason::DelaySlotEntry:
        return "delay-slot-entry";
    case GuardedAotEntryRejectionReason::UnknownInstruction:
        return "unknown-instruction";
    case GuardedAotEntryRejectionReason::EntryExtentUnavailable:
        return "entry-extent-unavailable";
    case GuardedAotEntryRejectionReason::SourceByteOffsetUnavailable:
        return "source-byte-offset-unavailable";
    case GuardedAotEntryRejectionReason::SegmentByteOffsetUnavailable:
        return "segment-byte-offset-unavailable";
    case GuardedAotEntryRejectionReason::EntryBytesUnavailable:
        return "entry-bytes-unavailable";
    }
    return "unknown";
}

const char*
exact_guard_rejection_reason_name(
    const ExactGuardRejectionReason reason) noexcept {
    switch (reason) {
    case ExactGuardRejectionReason::None:
        return "none";
    case ExactGuardRejectionReason::PrecedingCall:
        return "preceding-call";
    case ExactGuardRejectionReason::ConflictingTargets:
        return "conflicting-targets";
    case ExactGuardRejectionReason::TargetNotExecutable:
        return "target-not-executable";
    case ExactGuardRejectionReason::UnknownTargetOpcode:
        return "unknown-target-opcode";
    case ExactGuardRejectionReason::DispatchImmutableProofMissing:
        return "dispatch-immutable-proof-missing";
    case ExactGuardRejectionReason::DefinitionImmutableProofMissing:
        return "definition-immutable-proof-missing";
    case ExactGuardRejectionReason::WriterImmutableProofMissing:
        return "writer-immutable-proof-missing";
    case ExactGuardRejectionReason::LiteralImmutableProofMissing:
        return "literal-immutable-proof-missing";
    case ExactGuardRejectionReason::TargetImmutableProofMissing:
        return "target-immutable-proof-missing";
    case ExactGuardRejectionReason::ImageIdentityMismatch:
        return "image-identity-mismatch";
    case ExactGuardRejectionReason::Count:
        break;
    }
    return "unknown";
}

} // namespace katana::analysis
