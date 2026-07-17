#include "katana/analysis/control_flow_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
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

bool add_seed(std::map<std::uint32_t, std::set<FunctionOrigin>>& seeds,
              const std::uint32_t address,
              const std::span<const FunctionOrigin> origins = {}) {
    const auto [iterator, inserted] = seeds.try_emplace(address);
    bool changed = inserted;
    for (const auto origin : origins) {
        changed = iterator->second.insert(origin).second || changed;
    }
    return changed;
}

bool add_resolution_seeds(std::map<std::uint32_t, std::set<FunctionOrigin>>& seeds,
                          const IndirectControlFlowResolution& resolution) {
    if (resolution.status != ResolutionStatus::Resolved) return false;
    auto targets = resolution.targets;
    if (resolution.target.has_value()) targets.push_back(*resolution.target);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    bool changed = false;
    for (const auto target : targets) {
        if (resolution.kind != IndirectControlFlowKind::Call) {
            changed = add_seed(seeds, target) || changed;
            continue;
        }
        if (resolution.reason == "user-override" || resolution.reason == "user-hint") {
            const std::array origins{FunctionOrigin::IndirectCall,
                                     resolution.reason == "user-hint"
                                         ? FunctionOrigin::UserHint
                                         : FunctionOrigin::UserOverride};
            changed = add_seed(seeds, target, origins) || changed;
        } else {
            const std::array origins{FunctionOrigin::IndirectCall};
            changed = add_seed(seeds, target, origins) || changed;
        }
    }
    return changed;
}

RecursiveAnalysisOptions
make_options(const std::map<std::uint32_t, std::set<FunctionOrigin>>& seeds) {
    RecursiveAnalysisOptions options;
    options.additional_seeds.reserve(seeds.size());
    for (const auto& [address, origins] : seeds) {
        AnalysisSeed seed;
        seed.address = address;
        seed.function_origins.assign(origins.begin(), origins.end());
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
    resolution->target.reset();
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

void classify_dynamic_sites(const std::span<const katana::sh4::DisassemblyLine> lines,
                            std::vector<IndirectControlFlowResolution>& resolutions) {
    for (auto& resolution : resolutions) {
        if (resolution.status == ResolutionStatus::Resolved) continue;
        const auto dispatch = std::lower_bound(
            lines.begin(),
            lines.end(),
            resolution.instruction_address,
            [](const auto& line, const auto address) { return line.address < address; });
        if (dispatch == lines.end() || dispatch->address != resolution.instruction_address)
            continue;
        const auto index = static_cast<std::size_t>(dispatch - lines.begin());
        const auto first = index > 8u ? index - 8u : 0u;
        const katana::sh4::DisassemblyLine* writer = nullptr;
        const katana::sh4::DisassemblyLine* base_writer = nullptr;
        bool preceding_call = false;
        for (std::size_t current = index; current-- > first;) {
            if (lines[current].address + 2u != lines[current + 1u].address) break;
            const auto& instruction = lines[current].instruction;
            if (instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall)
                preceding_call = true;
            if (writer == nullptr &&
                (general_register_write_mask(instruction) &
                 static_cast<std::uint16_t>(1u << resolution.register_index)) != 0u) {
                if (instruction.destination_register == resolution.register_index &&
                    memory_load(instruction.kind)) {
                    writer = &lines[current];
                    continue;
                }
                break;
            }
            if (writer != nullptr && memory_load(instruction.kind) &&
                instruction.destination_register == writer->instruction.source_register) {
                base_writer = &lines[current];
                break;
            }
        }
        if (resolution.register_index == 0u && preceding_call) {
            resolution.reason = "dynamic-return-value";
        } else if (resolution.register_index == 15u ||
                   (writer != nullptr && writer->instruction.source_register == 15u)) {
            resolution.reason = "dynamic-stack-target";
        } else if (writer != nullptr && base_writer != nullptr) {
            resolution.reason = "dynamic-vtable-target";
        } else if (writer != nullptr) {
            resolution.reason = "dynamic-unbounded-memory";
        } else if (resolution.register_index >= 4u && resolution.register_index <= 7u) {
            resolution.reason = "dynamic-parameter";
        }
    }
}

std::vector<ResolvedControlFlowEdge>
collect_resolved_edges(const std::span<const IndirectControlFlowResolution> resolutions,
                       const std::span<const JumpTableAnalysis> tables) {
    std::vector<ResolvedControlFlowEdge> edges;
    for (const auto& resolution : resolutions) {
        if (resolution.status != ResolutionStatus::Resolved) continue;
        std::vector<std::uint32_t> targets = resolution.targets;
        if (resolution.target.has_value()) targets.push_back(*resolution.target);
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (const auto target : targets) {
            edges.push_back({resolution.instruction_address,
                             target,
                             resolution.kind == IndirectControlFlowKind::Call
                                 ? ResolvedControlFlowKind::Call
                                 : ResolvedControlFlowKind::Jump});
        }
    }
    for (const auto& table : tables) {
        if (!table.resolved) continue;
        for (const auto& entry : table.entries) {
            edges.push_back({table.dispatch_address,
                             entry.target,
                             table.dispatch_kind == JumpTableDispatchKind::Call
                                 ? ResolvedControlFlowKind::Call
                                 : ResolvedControlFlowKind::Jump});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const auto& left, const auto& right) {
        if (left.instruction_address != right.instruction_address)
            return left.instruction_address < right.instruction_address;
        if (left.target_address != right.target_address)
            return left.target_address < right.target_address;
        return left.kind < right.kind;
    });
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}

} // namespace

ControlFlowAnalysisResult analyze_control_flow(const katana::io::ExecutableImage& image,
                                               const AnalysisOverrides* overrides) {
    std::map<std::uint32_t, std::set<FunctionOrigin>> seeds;
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
            static_cast<void>(add_seed(seeds, function.address, origins));
            if (hints) {
                seed_diagnostics.push_back({function.line,
                                            function.address,
                                            AnalysisDirectiveDiagnosticStatus::Accepted,
                                            "function-seed"});
            }
        }
    }

    ControlFlowAnalysisResult analysis;
    for (;;) {
        ++analysis.fixpoint_iterations;
        analysis.recursive = analyze_reachable_code(image, make_options(seeds));
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
            if (directive_dispatches.contains(line.address) ||
                line.instruction.kind != katana::sh4::InstructionKind::Braf)
                continue;
            const auto found = resolution_by_address.find(line.address);
            if (found == resolution_by_address.end() ||
                analysis.indirect_control_flow[found->second].status !=
                    ResolutionStatus::Unresolved)
                continue;
            auto table = recognize_bounded_relative_jump_table(
                image, analysis.recursive.instructions, index);
            if (!table.has_value()) continue;
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
                    const bool confirmed = resolution->target == jump.target;
                    analysis.directive_diagnostics.push_back(
                        {jump.line,
                         jump.instruction_address,
                         confirmed ? AnalysisDirectiveDiagnosticStatus::Confirmed
                                   : AnalysisDirectiveDiagnosticStatus::Rejected,
                         confirmed ? "matches-static-proof" : "conflicts-with-static-proof"});
                    continue;
                }
                resolution->status = ResolutionStatus::Resolved;
                resolution->target = jump.target;
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
                    image, table.dispatch_address, table.table_address, table.entry_count);
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
                analysis.jump_tables.push_back(std::move(jump_table));
                mark_resolved_table_dispatch(analysis.indirect_control_flow,
                                             analysis.jump_tables.back());
            }
        }

        bool changed = false;
        for (const auto& resolution : analysis.indirect_control_flow) {
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
                    changed = add_seed(seeds, entry.target, origins) || changed;
                } else {
                    changed = add_seed(seeds, entry.target) || changed;
                }
            }
        }
        if (changed) {
            continue;
        }

        std::vector<std::uint32_t> function_entries;
        function_entries.reserve(analysis.recursive.functions.size());
        for (const auto& function : analysis.recursive.functions)
            function_entries.push_back(function.address);
        const auto provisional_edges =
            collect_resolved_edges(analysis.indirect_control_flow, analysis.jump_tables);
        auto function_values = analyze_function_values(
            image, analysis.recursive.instructions, function_entries, provisional_edges);
        analysis.function_value_summaries = std::move(function_values.summaries);
        for (auto& proof : function_values.resolutions) {
            const auto found = resolution_by_address.find(proof.instruction_address);
            if (found == resolution_by_address.end()) continue;
            auto resolution =
                analysis.indirect_control_flow.begin() + static_cast<std::ptrdiff_t>(found->second);
            if (resolution->status != ResolutionStatus::Unresolved) continue;
            resolution->status = ResolutionStatus::Resolved;
            resolution->target = proof.targets.size() == 1u
                                     ? std::optional<std::uint32_t>(proof.targets.front())
                                     : std::nullopt;
            resolution->reason = std::move(proof.reason);
            resolution->targets = std::move(proof.targets);
            resolution->evidence_call_sites = std::move(proof.call_sites);
            resolution->evidence_callees = std::move(proof.callees);
        }
        classify_dynamic_sites(analysis.recursive.instructions, analysis.indirect_control_flow);
        for (const auto& resolution : analysis.indirect_control_flow)
            changed = add_resolution_seeds(seeds, resolution) || changed;
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
    analysis.resolved_edges =
        collect_resolved_edges(analysis.indirect_control_flow, analysis.jump_tables);
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
