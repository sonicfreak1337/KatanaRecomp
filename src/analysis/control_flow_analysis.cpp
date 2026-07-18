#include "katana/analysis/control_flow_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace katana::analysis {
namespace {

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
};

bool add_seed(std::map<std::uint32_t, SeedEvidence>& seeds,
              const std::uint32_t address,
              const std::span<const FunctionOrigin> origins = {},
              const bool proven = true,
              const ControlFlowEvidence evidence = ControlFlowEvidence::ProvenComplete) {
    const auto [iterator, inserted] = seeds.try_emplace(address);
    bool changed = inserted;
    if (proven && !iterator->second.proven) {
        iterator->second.proven = true;
        changed = true;
    }
    if (control_flow_evidence_strength(evidence) >
        control_flow_evidence_strength(iterator->second.evidence)) {
        changed = iterator->second.evidence != evidence || changed;
        iterator->second.evidence = evidence;
    }
    for (const auto origin : origins) {
        changed = iterator->second.origins.insert(origin).second || changed;
    }
    return changed;
}

bool add_resolution_seeds(std::map<std::uint32_t, SeedEvidence>& seeds,
                          const IndirectControlFlowResolution& resolution) {
    auto targets = resolution.targets;
    if (resolution.target.has_value()) targets.push_back(*resolution.target);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.empty()) return false;
    bool changed = false;
    for (const auto target : targets) {
        if (resolution.kind != IndirectControlFlowKind::Call) {
            changed = add_seed(seeds,
                               target,
                               {},
                               control_flow_evidence_proven(resolution.evidence),
                               resolution.evidence) ||
                      changed;
            continue;
        }
        if (resolution.reason == "user-override" || resolution.reason == "user-hint") {
            const std::array origins{FunctionOrigin::IndirectCall,
                                     resolution.reason == "user-hint"
                                         ? FunctionOrigin::UserHint
                                         : FunctionOrigin::UserOverride};
            changed = add_seed(seeds, target, origins, false, resolution.evidence) || changed;
        } else if (resolution.status == ResolutionStatus::Guarded) {
            const std::array origins{FunctionOrigin::GuardedSnapshot};
            changed = add_seed(seeds, target, origins, false, resolution.evidence) || changed;
        } else {
            const std::array origins{FunctionOrigin::IndirectCall};
            changed = add_seed(seeds,
                               target,
                               origins,
                               control_flow_evidence_proven(resolution.evidence),
                               resolution.evidence) ||
                      changed;
        }
    }
    return changed;
}

RecursiveAnalysisOptions make_options(const std::map<std::uint32_t, SeedEvidence>& seeds) {
    RecursiveAnalysisOptions options;
    options.additional_seeds.reserve(seeds.size());
    for (const auto& [address, evidence] : seeds) {
        AnalysisSeed seed;
        seed.address = address;
        seed.function_origins.assign(evidence.origins.begin(), evidence.origins.end());
        seed.guarded_candidate = !evidence.proven;
        seed.evidence = evidence.evidence;
        options.additional_seeds.push_back(std::move(seed));
    }
    return options;
}

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
    if (!table.resolved) return;
    const auto resolution =
        std::find_if(resolutions.begin(), resolutions.end(), [&table](const auto& candidate) {
            return candidate.instruction_address == table.dispatch_address;
        });
    if (resolution == resolutions.end()) return;
    resolution->status = ResolutionStatus::Resolved;
    resolution->evidence = table.evidence;
    resolution->evidence_origins = {table.evidence == ControlFlowEvidence::HintCandidate
                                        ? AnalysisEvidenceOrigin::UserHint
                                    : table.evidence == ControlFlowEvidence::ForcedOverride
                                        ? AnalysisEvidenceOrigin::UserOverride
                                        : AnalysisEvidenceOrigin::JumpTable};
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
        return true;
    default:
        return false;
    }
}

struct BackwardSlice {
    std::set<std::uint32_t> writers;
    bool incomplete = false;
    bool preceding_call = false;
};

BackwardSlice bounded_writer_slice(const std::span<const BasicBlock> blocks,
                                   const std::uint32_t before_address,
                                   const std::uint8_t register_index) {
    std::unordered_map<std::uint32_t, const BasicBlock*> by_start;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> predecessors;
    const BasicBlock* initial = nullptr;
    std::size_t initial_index = 0u;
    for (const auto& block : blocks) {
        by_start.emplace(block.start_address, &block);
        for (const auto successor : block.successors)
            predecessors[successor].push_back(block.start_address);
        for (std::size_t index = 0u; index < block.lines.size(); ++index) {
            if (block.lines[index].address == before_address) {
                initial = &block;
                initial_index = index;
            }
        }
    }
    BackwardSlice result;
    if (initial == nullptr) {
        result.incomplete = true;
        return result;
    }
    struct Work {
        std::uint32_t block = 0u;
        std::size_t before_index = 0u;
        std::size_t depth = 0u;
    };
    std::deque<Work> pending{{initial->start_address, initial_index, 0u}};
    std::set<std::pair<std::uint32_t, std::size_t>> visited;
    constexpr std::size_t instruction_budget = 64u;
    while (!pending.empty()) {
        const auto work = pending.front();
        pending.pop_front();
        if (!visited.emplace(work.block, work.before_index).second) continue;
        const auto found = by_start.find(work.block);
        if (found == by_start.end()) {
            result.incomplete = true;
            continue;
        }
        const auto& block = *found->second;
        bool writer_found = false;
        auto depth = work.depth;
        for (std::size_t index = work.before_index; index-- > 0u;) {
            if (++depth > instruction_budget) {
                result.incomplete = true;
                writer_found = true;
                break;
            }
            const auto& line = block.lines[index];
            if (line.instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall)
                result.preceding_call = true;
            if ((general_register_write_mask(line.instruction) &
                 static_cast<std::uint16_t>(1u << register_index)) == 0u)
                continue;
            writer_found = true;
            if (line.instruction.destination_register == register_index &&
                memory_load(line.instruction.kind))
                result.writers.insert(line.address);
            else
                result.incomplete = true;
            break;
        }
        if (writer_found) continue;
        const auto incoming = predecessors.find(block.start_address);
        if (incoming == predecessors.end() || incoming->second.empty()) {
            result.incomplete = true;
            continue;
        }
        for (const auto predecessor : incoming->second) {
            const auto predecessor_block = by_start.find(predecessor);
            if (predecessor_block != by_start.end())
                pending.push_back({predecessor, predecessor_block->second->lines.size(), depth});
        }
    }
    return result;
}

void classify_dynamic_sites(const std::span<const katana::sh4::DisassemblyLine> lines,
                            std::vector<IndirectControlFlowResolution>& resolutions) {
    const auto blocks = build_basic_blocks(lines);
    for (auto& resolution : resolutions) {
        if (resolution.status != ResolutionStatus::Unresolved ||
            resolution.evidence == ControlFlowEvidence::HintCandidate)
            continue;
        const auto dispatch = std::lower_bound(
            lines.begin(),
            lines.end(),
            resolution.instruction_address,
            [](const auto& line, const auto address) { return line.address < address; });
        if (dispatch == lines.end() || dispatch->address != resolution.instruction_address)
            continue;
        const auto slice =
            bounded_writer_slice(blocks, resolution.instruction_address, resolution.register_index);
        const katana::sh4::DisassemblyLine* writer = nullptr;
        if (slice.writers.size() == 1u && !slice.incomplete) {
            const auto found_writer = std::lower_bound(
                lines.begin(),
                lines.end(),
                *slice.writers.begin(),
                [](const auto& line, const auto address) { return line.address < address; });
            if (found_writer != lines.end() && found_writer->address == *slice.writers.begin())
                writer = &*found_writer;
        }
        bool vtable_base = false;
        if (writer != nullptr) {
            const auto base =
                bounded_writer_slice(blocks, writer->address, writer->instruction.source_register);
            vtable_base = base.writers.size() == 1u && !base.incomplete;
        }
        if (resolution.register_index == 0u && slice.preceding_call) {
            resolution.reason = "dynamic-return-value";
        } else if (resolution.register_index == 15u ||
                   (writer != nullptr && writer->instruction.source_register == 15u)) {
            resolution.reason = "dynamic-stack-target";
        } else if (writer != nullptr && vtable_base) {
            resolution.reason = "dynamic-vtable-target";
        } else if (writer != nullptr) {
            resolution.reason = "dynamic-unbounded-memory";
        } else if (resolution.register_index >= 4u && resolution.register_index <= 7u) {
            resolution.reason = "dynamic-parameter";
        }
        if (resolution.reason.starts_with("dynamic-")) {
            resolution.evidence = ControlFlowEvidence::RuntimeOnly;
            resolution.evidence_origins = {AnalysisEvidenceOrigin::RuntimeClassification};
        }
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
        if (!table.resolved) continue;
        for (const auto& entry : table.entries) {
            edges.push_back({table.dispatch_address,
                             entry.target,
                             table.dispatch_kind == JumpTableDispatchKind::Call
                                 ? ResolvedControlFlowKind::Call
                                 : ResolvedControlFlowKind::Jump,
                             false,
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

} // namespace

ControlFlowAnalysisResult analyze_control_flow(const katana::io::ExecutableImage& image,
                                               const AnalysisOverrides* overrides) {
    std::map<std::uint32_t, SeedEvidence> seeds;
    const bool hints = overrides != nullptr && overrides->mode == AnalysisDirectiveMode::Hint;
    std::vector<AnalysisDirectiveDiagnostic> seed_diagnostics;
    if (overrides != nullptr) {
        for (const auto& function : overrides->functions) {
            const auto validation = validate_committed_code_address(image, function.address);
            if (!validation.valid()) {
                if (hints) {
                    seed_diagnostics.push_back({function.line,
                                                function.address,
                                                AnalysisDirectiveDiagnosticStatus::Rejected,
                                                code_address_status_name(validation.status)});
                    continue;
                }
                require_override_code_address(image, *overrides, function.line, function.address);
            }
            const std::array origins{hints ? FunctionOrigin::UserHint
                                           : FunctionOrigin::UserOverride};
            static_cast<void>(add_seed(seeds,
                                       function.address,
                                       origins,
                                       false,
                                       hints ? ControlFlowEvidence::HintCandidate
                                             : ControlFlowEvidence::ForcedOverride));
            if (hints) {
                seed_diagnostics.push_back({function.line,
                                            function.address,
                                            AnalysisDirectiveDiagnosticStatus::Accepted,
                                            "function-seed"});
            }
        }
    }

    ControlFlowAnalysisResult analysis;
    JumpTableSnapshotCache jump_table_cache;
    for (;;) {
        ++analysis.fixpoint_iterations;
        auto recursive_options = make_options(seeds);
        if (!analysis.recursive.contextual_instructions.empty()) {
            recursive_options.baseline = &analysis.recursive;
        }
        analysis.recursive = analyze_reachable_code(image, recursive_options);
        analysis.indirect_control_flow =
            resolve_indirect_control_flow(analysis.recursive.instructions, image);
        std::unordered_map<std::uint32_t, std::size_t> resolution_by_address;
        resolution_by_address.reserve(analysis.indirect_control_flow.size());
        for (std::size_t index = 0u; index < analysis.indirect_control_flow.size(); ++index)
            resolution_by_address.emplace(analysis.indirect_control_flow[index].instruction_address,
                                          index);
        analysis.jump_tables.clear();
        analysis.directive_diagnostics = seed_diagnostics;
        bool missing_override_dispatch = false;

        std::set<std::uint32_t> directive_dispatches;
        if (overrides != nullptr) {
            for (const auto& jump : overrides->jumps)
                directive_dispatches.insert(jump.instruction_address);
            for (const auto& table : overrides->jump_tables)
                directive_dispatches.insert(table.dispatch_address);
        }
        for (std::size_t index = 0u; index < analysis.recursive.instructions.size(); ++index) {
            const auto& line = analysis.recursive.instructions[index];
            if ((!hints && directive_dispatches.contains(line.address)) ||
                line.instruction.kind != katana::sh4::InstructionKind::Braf)
                continue;
            const auto found = resolution_by_address.find(line.address);
            if (found == resolution_by_address.end() ||
                analysis.indirect_control_flow[found->second].status !=
                    ResolutionStatus::Unresolved)
                continue;
            auto table = recognize_bounded_relative_jump_table(
                image, analysis.recursive.instructions, index, &jump_table_cache);
            if (!table.has_value()) continue;
            table->evidence = ControlFlowEvidence::ProvenComplete;
            analysis.jump_tables.push_back(std::move(*table));
            mark_resolved_table_dispatch(analysis.indirect_control_flow,
                                         analysis.jump_tables.back());
        }

        if (overrides != nullptr) {
            for (const auto& jump : overrides->jumps) {
                const auto resolution = std::find_if(analysis.indirect_control_flow.begin(),
                                                     analysis.indirect_control_flow.end(),
                                                     [&jump](const auto& candidate) {
                                                         return candidate.instruction_address ==
                                                                jump.instruction_address;
                                                     });
                if (resolution == analysis.indirect_control_flow.end()) {
                    missing_override_dispatch = true;
                    continue;
                }
                const auto target_validation = validate_committed_code_address(image, jump.target);
                if (!target_validation.valid()) {
                    if (hints) {
                        analysis.directive_diagnostics.push_back(
                            {jump.line,
                             jump.instruction_address,
                             AnalysisDirectiveDiagnosticStatus::Rejected,
                             code_address_status_name(target_validation.status)});
                        continue;
                    }
                    require_override_code_address(image, *overrides, jump.line, jump.target);
                }
                if (hints && resolution->status == ResolutionStatus::Resolved) {
                    const bool confirmed = resolution->target == jump.target ||
                                           std::find(resolution->targets.begin(),
                                                     resolution->targets.end(),
                                                     jump.target) != resolution->targets.end();
                    analysis.directive_diagnostics.push_back(
                        {jump.line,
                         jump.instruction_address,
                         confirmed ? AnalysisDirectiveDiagnosticStatus::Confirmed
                                   : AnalysisDirectiveDiagnosticStatus::Rejected,
                         confirmed ? "matches-static-proof" : "conflicts-with-static-proof"});
                    continue;
                }
                if (hints) {
                    if (resolution->target.has_value())
                        resolution->targets.push_back(*resolution->target);
                    resolution->targets.push_back(jump.target);
                    std::sort(resolution->targets.begin(), resolution->targets.end());
                    resolution->targets.erase(
                        std::unique(resolution->targets.begin(), resolution->targets.end()),
                        resolution->targets.end());
                    resolution->target = jump.target;
                    if (control_flow_evidence_strength(ControlFlowEvidence::HintCandidate) >
                        control_flow_evidence_strength(resolution->evidence))
                        resolution->evidence = ControlFlowEvidence::HintCandidate;
                    resolution->evidence_origins.push_back(AnalysisEvidenceOrigin::UserHint);
                } else {
                    resolution->status = ResolutionStatus::Guarded;
                    resolution->evidence = ControlFlowEvidence::ForcedOverride;
                    resolution->targets.clear();
                    resolution->target = jump.target;
                    resolution->evidence_origins = {AnalysisEvidenceOrigin::UserOverride};
                }
                std::sort(resolution->evidence_origins.begin(), resolution->evidence_origins.end());
                resolution->evidence_origins.erase(std::unique(resolution->evidence_origins.begin(),
                                                               resolution->evidence_origins.end()),
                                                   resolution->evidence_origins.end());
                resolution->reason = hints ? "user-hint" : "user-override";
                if (hints) {
                    analysis.directive_diagnostics.push_back(
                        {jump.line,
                         jump.instruction_address,
                         AnalysisDirectiveDiagnosticStatus::Accepted,
                         "resolved-unproven-target"});
                }
            }

            for (const auto& table : overrides->jump_tables) {
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
                const auto* dispatch = find_instruction(analysis.recursive, table.dispatch_address);
                if (dispatch == nullptr) {
                    missing_override_dispatch = true;
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
                const auto proven = std::find_if(
                    analysis.indirect_control_flow.begin(),
                    analysis.indirect_control_flow.end(),
                    [&table](const auto& candidate) {
                        return candidate.instruction_address == table.dispatch_address &&
                               candidate.status == ResolutionStatus::Resolved;
                    });
                if (hints && proven != analysis.indirect_control_flow.end()) {
                    analysis.directive_diagnostics.push_back(
                        {table.line,
                         table.dispatch_address,
                         AnalysisDirectiveDiagnosticStatus::Rejected,
                         "static-target-already-proven"});
                    continue;
                }
                auto jump_table = analyze_jump_table(
                    image,
                    table.dispatch_address,
                    table.table_address,
                    table.entry_count,
                    &jump_table_cache);
                jump_table.dispatch_kind =
                    (dispatch->instruction.kind == katana::sh4::InstructionKind::Jsr ||
                     dispatch->instruction.kind == katana::sh4::InstructionKind::Bsrf)
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
                jump_table.evidence = hints ? ControlFlowEvidence::HintCandidate
                                            : ControlFlowEvidence::ForcedOverride;
                analysis.jump_tables.push_back(std::move(jump_table));
                mark_resolved_table_dispatch(analysis.indirect_control_flow,
                                             analysis.jump_tables.back());
            }
        }

        const auto is_jump_table_dispatch = [&analysis](const std::uint32_t address) {
            return std::ranges::any_of(analysis.jump_tables, [address](const auto& table) {
                return table.dispatch_address == address;
            });
        };
        bool changed = false;
        for (const auto& resolution : analysis.indirect_control_flow) {
            if (is_jump_table_dispatch(resolution.instruction_address)) continue;
            changed = add_resolution_seeds(seeds, resolution) || changed;
        }
        for (const auto& table : analysis.jump_tables) {
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
                                       table.evidence) ||
                              changed;
                } else {
                    changed = add_seed(seeds,
                                       entry.target,
                                       {},
                                       control_flow_evidence_proven(table.evidence),
                                       table.evidence) ||
                              changed;
                }
            }
        }
        if (changed) {
            continue;
        }

        std::vector<std::uint32_t> function_entries;
        function_entries.reserve(analysis.recursive.functions.size());
        for (const auto& function : analysis.recursive.functions) {
            if (function.evidence != ControlFlowEvidence::Unresolved)
                function_entries.push_back(function.address);
        }
        const auto provisional_edges =
            collect_resolved_edges(analysis.indirect_control_flow, analysis.jump_tables);
        auto function_values = analyze_function_values(
            image, analysis.recursive.instructions, function_entries, provisional_edges);
        analysis.function_summary_iterations = function_values.fixpoint_iterations;
        analysis.function_scc_count = function_values.strongly_connected_components;
        analysis.unchanged_ingress_skips = function_values.unchanged_ingress_skips;
        analysis.function_value_summaries = std::move(function_values.summaries);
        for (auto& proof : function_values.resolutions) {
            if (proof.targets.empty()) continue;
            const auto found = resolution_by_address.find(proof.instruction_address);
            if (found == resolution_by_address.end()) continue;
            auto resolution =
                analysis.indirect_control_flow.begin() + static_cast<std::ptrdiff_t>(found->second);
            if (resolution->status == ResolutionStatus::Resolved ||
                resolution->evidence == ControlFlowEvidence::ForcedOverride)
                continue;
            resolution->status =
                proof.guarded ? ResolutionStatus::Guarded : ResolutionStatus::Resolved;
            resolution->evidence = proof.complete && !proof.guarded
                                       ? ControlFlowEvidence::ProvenComplete
                                       : proof.evidence;
            resolution->evidence_origins = {AnalysisEvidenceOrigin::FunctionSummary};
            resolution->target = proof.targets.size() == 1u
                                     ? std::optional<std::uint32_t>(proof.targets.front())
                                     : std::nullopt;
            resolution->reason = std::move(proof.reason);
            resolution->targets = std::move(proof.targets);
            resolution->evidence_call_sites = std::move(proof.call_sites);
            resolution->evidence_callees = std::move(proof.callees);
        }
        classify_dynamic_sites(analysis.recursive.instructions, analysis.indirect_control_flow);
        for (const auto& resolution : analysis.indirect_control_flow) {
            if (is_jump_table_dispatch(resolution.instruction_address)) continue;
            changed = add_resolution_seeds(seeds, resolution) || changed;
        }
        if (!changed && missing_override_dispatch && overrides != nullptr) {
            for (const auto& jump : overrides->jumps) {
                const auto resolution = std::find_if(analysis.indirect_control_flow.begin(),
                                                     analysis.indirect_control_flow.end(),
                                                     [&jump](const auto& candidate) {
                                                         return candidate.instruction_address ==
                                                                jump.instruction_address;
                                                     });
                if (resolution == analysis.indirect_control_flow.end()) {
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
                if (find_instruction(analysis.recursive, table.dispatch_address) == nullptr) {
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
    for (auto& resolution : analysis.indirect_control_flow) {
        if (!control_flow_evidence_proven(resolution.evidence)) continue;
        auto targets = resolution.targets;
        if (resolution.target.has_value()) targets.push_back(*resolution.target);
        const bool boundaries = std::all_of(targets.begin(), targets.end(), [&](const auto target) {
            return proven_instruction_boundary(analysis.recursive.proven_instruction_addresses,
                                               target);
        });
        if (!boundaries) {
            resolution.status = ResolutionStatus::Guarded;
            resolution.evidence = ControlFlowEvidence::GuardedPartial;
            resolution.reason += "-decode-candidate-only";
        }
    }
    for (auto& table : analysis.jump_tables) {
        if (!table.resolved || !control_flow_evidence_proven(table.evidence)) continue;
        const bool boundaries =
            std::all_of(table.entries.begin(), table.entries.end(), [&](const auto& entry) {
                return proven_instruction_boundary(analysis.recursive.proven_instruction_addresses,
                                                   entry.target);
            });
        if (!boundaries) table.evidence = ControlFlowEvidence::GuardedPartial;
    }
    analysis.resolved_edges =
        collect_resolved_edges(analysis.indirect_control_flow, analysis.jump_tables);
    analysis.sites.reserve(analysis.indirect_control_flow.size());
    for (const auto& resolution : analysis.indirect_control_flow) {
        ControlFlowSite site;
        site.instruction_address = resolution.instruction_address;
        site.kind = resolution.kind;
        site.evidence = resolution.evidence;
        site.evidence_origins = resolution.evidence_origins;
        site.targets = resolution.targets;
        if (resolution.target.has_value()) site.targets.push_back(*resolution.target);
        std::sort(site.targets.begin(), site.targets.end());
        site.targets.erase(std::unique(site.targets.begin(), site.targets.end()),
                           site.targets.end());
        site.evidence_call_sites = resolution.evidence_call_sites;
        site.evidence_callees = resolution.evidence_callees;
        analysis.sites.push_back(std::move(site));
    }
    std::sort(
        analysis.sites.begin(), analysis.sites.end(), [](const auto& left, const auto& right) {
            return left.instruction_address < right.instruction_address;
        });
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
    for (const auto& resolution : analysis.indirect_control_flow) {
        symbolic_candidates.insert(resolution.instruction_address);
        if (resolution.target.has_value()) symbolic_candidates.insert(*resolution.target);
        symbolic_candidates.insert(resolution.targets.begin(), resolution.targets.end());
        symbolic_candidates.insert(resolution.evidence_call_sites.begin(),
                                   resolution.evidence_call_sites.end());
        symbolic_candidates.insert(resolution.evidence_callees.begin(),
                                   resolution.evidence_callees.end());
    }
    for (const auto& table : analysis.jump_tables) {
        symbolic_candidates.insert(table.dispatch_address);
        symbolic_candidates.insert(table.table_address);
        for (const auto& entry : table.entries)
            symbolic_candidates.insert(entry.target);
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
    std::vector<std::uint32_t> final_function_entries;
    final_function_entries.reserve(analysis.recursive.functions.size());
    for (const auto& function : analysis.recursive.functions) {
        final_function_entries.push_back(function.address);
    }
    const auto final_blocks = build_basic_blocks(
        analysis.recursive.instructions, analysis.resolved_edges, final_function_entries);
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
    for (const auto& table : analysis.jump_tables) {
        evidence_strings.push_back(table.reason);
        for (const auto& entry : table.entries)
            evidence_strings.push_back(entry.reason);
    }
    std::sort(evidence_strings.begin(), evidence_strings.end());
    evidence_strings.erase(std::unique(evidence_strings.begin(), evidence_strings.end()),
                           evidence_strings.end());
    for (const auto& evidence : evidence_strings)
        static_cast<void>(analysis.evidence_ids.intern(evidence));
    analysis.jump_table_cache = jump_table_cache.counters();
    return analysis;
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

} // namespace katana::analysis
