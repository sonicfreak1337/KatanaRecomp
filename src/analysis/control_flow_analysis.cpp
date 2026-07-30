#include "katana/analysis/control_flow_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/instruction.hpp"
#include "guarded_native_entry_shape.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
};

bool add_seed(std::map<std::uint32_t, SeedEvidence>& seeds,
              const std::uint32_t address,
              const std::span<const FunctionOrigin> origins = {},
              const bool proven = true,
              const ControlFlowEvidence evidence =
                  ControlFlowEvidence::ProvenComplete,
              const std::uint32_t function_size = 0u) {
    const auto [iterator, inserted] = seeds.try_emplace(address);
    bool changed = inserted;
    if (function_size != 0u) {
        if (iterator->second.function_size != 0u &&
            iterator->second.function_size != function_size)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen widersprechen sich.");
        if (iterator->second.function_size == 0u) {
            iterator->second.function_size = function_size;
            changed = true;
        }
    }
    if (proven && !iterator->second.proven) {
        iterator->second.proven = true;
        changed = true;
    }
    if (control_flow_evidence_preferred_for_static_decode(evidence,
                                                         iterator->second.evidence)) {
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
    targets.insert(targets.end(),
                   resolution.analysis_candidates.begin(),
                   resolution.analysis_candidates.end());
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.empty()) return false;
    bool changed = false;
    const bool relative_call_island =
        resolution.reason == "runtime-contract-snapshot-relative-call-island-candidates";
    for (const auto target : targets) {
        if (relative_call_island) {
            changed = add_seed(seeds,
                               target,
                               {},
                               false,
                               ControlFlowEvidence::GuardedPartial) ||
                      changed;
            continue;
        }
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
        seed.function_size = evidence.function_size;
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

struct WriterSliceLocation {
    std::uint32_t block = 0u;
    std::size_t before_index = 0u;
};

struct WriterSliceIndex {
    std::unordered_map<std::uint32_t, const BasicBlock*> by_start;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> predecessors;
    std::unordered_map<std::uint32_t, WriterSliceLocation> locations;
};

WriterSliceIndex build_writer_slice_index(const std::span<const BasicBlock> blocks) {
    WriterSliceIndex index;
    index.by_start.reserve(blocks.size());
    index.predecessors.reserve(blocks.size());
    std::size_t instruction_count = 0u;
    for (const auto& block : blocks)
        instruction_count += block.lines.size();
    index.locations.reserve(instruction_count);
    for (const auto& block : blocks) {
        index.by_start.emplace(block.start_address, &block);
        for (const auto successor : block.successors)
            index.predecessors[successor].push_back(block.start_address);
        for (std::size_t line_index = 0u; line_index < block.lines.size(); ++line_index) {
            index.locations.insert_or_assign(
                block.lines[line_index].address,
                WriterSliceLocation{block.start_address, line_index});
        }
    }
    return index;
}

BackwardSlice bounded_writer_slice(const WriterSliceIndex& index,
                                   const std::uint32_t before_address,
                                   const std::uint8_t register_index) {
    BackwardSlice result;
    const auto initial = index.locations.find(before_address);
    if (initial == index.locations.end()) {
        result.incomplete = true;
        return result;
    }
    struct Work {
        std::uint32_t block = 0u;
        std::size_t before_index = 0u;
        std::size_t depth = 0u;
    };
    std::deque<Work> pending{
        {initial->second.block, initial->second.before_index, 0u}};
    std::set<std::pair<std::uint32_t, std::size_t>> visited;
    constexpr std::size_t instruction_budget = 64u;
    while (!pending.empty()) {
        const auto work = pending.front();
        pending.pop_front();
        if (!visited.emplace(work.block, work.before_index).second) continue;
        const auto found = index.by_start.find(work.block);
        if (found == index.by_start.end()) {
            result.incomplete = true;
            continue;
        }
        const auto& block = *found->second;
        bool writer_found = false;
        auto depth = work.depth;
        for (std::size_t line_index = work.before_index; line_index-- > 0u;) {
            if (++depth > instruction_budget) {
                result.incomplete = true;
                writer_found = true;
                break;
            }
            const auto& line = block.lines[line_index];
            if (line.instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall)
                result.preceding_call = true;
            if ((general_register_write_mask(line.instruction) &
                 static_cast<std::uint16_t>(1u << register_index)) == 0u)
                continue;
            writer_found = true;
            result.writers.insert(line.address);
            if (line.instruction.destination_register != register_index ||
                !memory_load(line.instruction.kind))
                result.incomplete = true;
            break;
        }
        if (writer_found) continue;
        const auto incoming = index.predecessors.find(block.start_address);
        if (incoming == index.predecessors.end() || incoming->second.empty()) {
            result.incomplete = true;
            continue;
        }
        for (const auto predecessor : incoming->second) {
            const auto predecessor_block = index.by_start.find(predecessor);
            if (predecessor_block != index.by_start.end())
                pending.push_back({predecessor, predecessor_block->second->lines.size(), depth});
        }
    }
    return result;
}

void classify_dynamic_sites(const std::span<const katana::sh4::DisassemblyLine> lines,
                            std::vector<IndirectControlFlowResolution>& resolutions) {
    const auto blocks = build_basic_blocks(lines);
    const auto writer_slice_index = build_writer_slice_index(blocks);
    for (auto& resolution : resolutions) {
        if (resolution.origin_class != IndirectControlFlowOriginClass::Table &&
            resolution.status == ResolutionStatus::Resolved &&
            control_flow_evidence_complete(resolution.evidence))
            continue;
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
        resolution.definition_complete = !slice.incomplete && !slice.writers.empty();
        resolution.preceding_call = slice.preceding_call;
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
        if (resolution.origin_class == IndirectControlFlowOriginClass::Table) continue;
        bool vtable_base = false;
        bool stack_base = false;
        bool object_field = false;
        bool callback_source = resolution.register_index == 13u;
        if (writer != nullptr) {
            const auto base = bounded_writer_slice(
                writer_slice_index, writer->address, writer->instruction.source_register);
            vtable_base = base.writers.size() == 1u && !base.incomplete;
            callback_source = callback_source || (writer->instruction.kind ==
                                                      katana::sh4::InstructionKind::MovRegister &&
                                                  writer->instruction.source_register == 13u);
            const auto writer_kind = writer->instruction.kind;
            object_field = writer->instruction.source_register >= 4u &&
                           writer->instruction.source_register <= 14u &&
                           (writer_kind == katana::sh4::InstructionKind::MovLongLoad ||
                            writer_kind == katana::sh4::InstructionKind::MovLongLoadDisplacement);
            if (vtable_base) {
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

void bind_partial_runtime_contracts(std::vector<IndirectControlFlowResolution>& resolutions) {
    for (auto& resolution : resolutions) {
        if (resolution.evidence != ControlFlowEvidence::GuardedPartial) continue;
        const bool guarded_memory = resolution.reason == "guarded-function-memory";
        const bool merged_contexts = resolution.reason == "merged-contexts-partial";
        const bool inventory_code_pointer_set =
            resolution.reason == "inventory-code-pointer-set";
        const bool writable_literal =
            resolution.reason.find("guarded-writable-pc-relative-literal") != std::string::npos;
        if (!guarded_memory && !merged_contexts &&
            !inventory_code_pointer_set && !writable_literal)
            continue;
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

std::vector<ResolvedControlFlowEdge>
collect_function_value_edges(
    const std::span<const IndirectControlFlowResolution> resolutions,
    const std::span<const JumpTableAnalysis> tables) {
    auto edges = collect_resolved_edges(resolutions, tables);
    // Candidate-only transfers must not become executable CFG edges.  This
    // private edge set uses Call as a non-successor carrier for finite calls
    // and non-table jumps; the value analyzer inspects the real opcode and
    // reclassifies Jmp/Braf sites as guarded tail ingress.  GuardedPartial
    // keeps unknown live targets authoritative.
    for (const auto& resolution : resolutions) {
        if (resolution.kind != IndirectControlFlowKind::Call &&
            resolution.kind != IndirectControlFlowKind::Jump)
            continue;
        if (resolution.kind == IndirectControlFlowKind::Jump &&
            resolution.origin_class == IndirectControlFlowOriginClass::Table)
            continue;
        for (const auto target : resolution.analysis_candidates) {
            edges.push_back({resolution.instruction_address,
                             target,
                             ResolvedControlFlowKind::Call,
                             true,
                             ControlFlowEvidence::GuardedPartial,
                             resolution.evidence_origins,
                             true});
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
        if (left.evidence_origins != right.evidence_origins)
            return left.evidence_origins < right.evidence_origins;
        return left.analysis_candidate_carrier <
               right.analysis_candidate_carrier;
    });
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}

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

ControlFlowAnalysisResult analyze_control_flow(const katana::io::ExecutableImage& image,
                                               const AnalysisOverrides* overrides,
                                               const ControlFlowAnalysisProgressCallback&
                                                   progress_callback) {
    std::map<std::uint32_t, SeedEvidence> seeds;
    const bool hints = overrides != nullptr && overrides->mode == AnalysisDirectiveMode::Hint;
    std::vector<AnalysisDirectiveDiagnostic> seed_diagnostics;
    if (overrides != nullptr) {
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
            static_cast<void>(add_seed(seeds,
                                       validation.resolved_address,
                                       origins,
                                       false,
                                       hints ? ControlFlowEvidence::HintCandidate
                                             : ControlFlowEvidence::ForcedOverride,
                                       function.size));
            if (hints) {
                seed_diagnostics.push_back({function.line,
                                            function.address,
                                            AnalysisDirectiveDiagnosticStatus::Accepted,
                                            "function-seed"});
            }
        }
    }

    ControlFlowAnalysisResult analysis;
    GuardedCodeInventory final_guarded_code_inventory;
    detail::GuardedNativeEntryShapeCache guarded_native_entry_shapes(image);
    JumpTableSnapshotCache jump_table_cache;
    const auto report_progress = [&](const std::string_view phase) {
        if (!progress_callback) return;
        progress_callback({phase,
                           analysis.fixpoint_iterations,
                           seeds.size(),
                           analysis.recursive.instructions.size(),
                           analysis.recursive.contextual_instructions.size(),
                           analysis.indirect_control_flow.size()});
    };
    const auto apply_decode_boundary_downgrades = [&] {
        bool changed = false;
        for (auto& resolution : analysis.indirect_control_flow) {
            if (!control_flow_evidence_proven(resolution.evidence)) continue;
            auto targets = resolution.targets;
            if (resolution.target.has_value())
                targets.push_back(*resolution.target);
            const bool boundaries =
                std::all_of(targets.begin(), targets.end(), [&](const auto target) {
                    return proven_instruction_boundary(
                        analysis.recursive.proven_instruction_addresses,
                        target);
                });
            if (boundaries) continue;
            resolution.status = ResolutionStatus::Guarded;
            resolution.evidence = ControlFlowEvidence::GuardedPartial;
            if (!resolution.reason.ends_with("-decode-candidate-only"))
                resolution.reason += "-decode-candidate-only";
            changed = true;
        }
        for (auto& table : analysis.jump_tables) {
            if (!table.resolved ||
                !control_flow_evidence_proven(table.evidence))
                continue;
            const bool boundaries =
                std::all_of(table.entries.begin(),
                            table.entries.end(),
                            [&](const auto& entry) {
                                return proven_instruction_boundary(
                                    analysis.recursive
                                        .proven_instruction_addresses,
                                    entry.target);
                            });
            if (boundaries) continue;
            table.evidence = ControlFlowEvidence::GuardedPartial;
            const auto resolution =
                std::find_if(analysis.indirect_control_flow.begin(),
                             analysis.indirect_control_flow.end(),
                             [&table](const auto& candidate) {
                                 return candidate.instruction_address ==
                                        table.dispatch_address;
                             });
            if (resolution != analysis.indirect_control_flow.end()) {
                resolution->status = ResolutionStatus::Guarded;
                resolution->evidence = ControlFlowEvidence::GuardedPartial;
                resolution->origin_class =
                    IndirectControlFlowOriginClass::Table;
            }
            changed = true;
        }
        return changed;
    };
    for (;;) {
        ++analysis.fixpoint_iterations;
        report_progress("iteration-start");
        auto recursive_options = make_options(seeds);
        if (!analysis.recursive.contextual_instructions.empty()) {
            recursive_options.baseline = &analysis.recursive;
        }
        analysis.recursive = analyze_reachable_code(image, recursive_options);
        analysis.runtime_code_copies =
            analyze_runtime_code_copies(image, analysis.recursive.instructions);
        report_progress("recursive-complete");
        auto local_control_flow =
            analyze_local_control_flow(analysis.recursive.instructions, image);
        analysis.indirect_control_flow = std::move(local_control_flow.indirect_control_flow);
        for (auto& continuation : local_control_flow.static_return_continuations) {
            const auto duplicate = std::find_if(
                analysis.static_return_continuations.begin(),
                analysis.static_return_continuations.end(),
                [&continuation](const auto& existing) {
                    return existing.instruction_address == continuation.instruction_address &&
                           existing.target_address == continuation.target_address;
                });
            if (duplicate == analysis.static_return_continuations.end())
                analysis.static_return_continuations.push_back(std::move(continuation));
        }
        std::sort(analysis.static_return_continuations.begin(),
                  analysis.static_return_continuations.end(),
                  [](const auto& left, const auto& right) {
                      if (left.instruction_address != right.instruction_address)
                          return left.instruction_address < right.instruction_address;
                      return left.target_address < right.target_address;
                  });
        report_progress("local-resolution-complete");
        std::unordered_map<std::uint32_t, std::size_t> resolution_by_address;
        resolution_by_address.reserve(analysis.indirect_control_flow.size());
        for (std::size_t index = 0u; index < analysis.indirect_control_flow.size(); ++index)
            resolution_by_address.emplace(analysis.indirect_control_flow[index].instruction_address,
                                          index);
        analysis.jump_tables.clear();
        analysis.directive_diagnostics = seed_diagnostics;
        bool missing_override_dispatch = false;
        std::unordered_set<std::uint32_t> snapshot_candidate_dispatches;

        std::set<std::uint32_t> directive_dispatches;
        if (overrides != nullptr) {
            for (const auto& jump : overrides->jumps)
                directive_dispatches.insert(jump.instruction_address);
            for (const auto& table : overrides->jump_tables)
                directive_dispatches.insert(table.dispatch_address);
        }
        for (std::size_t index = 0u; index < analysis.recursive.instructions.size(); ++index) {
            const auto& line = analysis.recursive.instructions[index];
            if (!hints && directive_dispatches.contains(line.address)) continue;
            const auto found = resolution_by_address.find(line.address);
            if (found == resolution_by_address.end() ||
                analysis.indirect_control_flow[found->second].status !=
                    ResolutionStatus::Unresolved)
                continue;
            if (line.instruction.kind == katana::sh4::InstructionKind::Bsrf) {
                const auto island = recognize_snapshot_relative_call_island_candidates(
                    image, analysis.recursive.instructions, index);
                if (island.has_value()) {
                    auto& resolution = analysis.indirect_control_flow[found->second];
                    resolution.status = ResolutionStatus::Unresolved;
                    resolution.evidence = ControlFlowEvidence::RuntimeOnly;
                    resolution.origin_class = IndirectControlFlowOriginClass::Table;
                    resolution.evidence_origins = {AnalysisEvidenceOrigin::EntrySnapshot,
                                                   AnalysisEvidenceOrigin::RuntimeClassification};
                    resolution.target.reset();
                    resolution.targets.clear();
                    resolution.analysis_candidates = island->targets;
                    std::sort(resolution.analysis_candidates.begin(),
                              resolution.analysis_candidates.end());
                    resolution.analysis_candidates.erase(
                        std::unique(resolution.analysis_candidates.begin(),
                                    resolution.analysis_candidates.end()),
                        resolution.analysis_candidates.end());
                    resolution.reason = "runtime-contract-" + island->reason;
                    snapshot_candidate_dispatches.insert(line.address);
                    continue;
                }
            }
            std::optional<JumpTableAnalysis> table;
            if (line.instruction.kind == katana::sh4::InstructionKind::Braf) {
                table = recognize_bounded_relative_jump_table(
                    image, analysis.recursive.instructions, index, &jump_table_cache);
            } else if (line.instruction.kind == katana::sh4::InstructionKind::Jmp ||
                       line.instruction.kind == katana::sh4::InstructionKind::Jsr) {
                table = recognize_snapshot_absolute_jump_table_candidates(
                    image, analysis.recursive.instructions, index);
            }
            if (!table.has_value()) continue;
            if (table->evidence == ControlFlowEvidence::Unresolved)
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
                jump_table.evidence = hints ? ControlFlowEvidence::HintCandidate
                                            : ControlFlowEvidence::ForcedOverride;
                analysis.jump_tables.push_back(std::move(jump_table));
                mark_resolved_table_dispatch(analysis.indirect_control_flow,
                                             analysis.jump_tables.back());
            }
        }

        std::unordered_set<std::uint32_t> jump_table_dispatches;
        jump_table_dispatches.reserve(analysis.jump_tables.size());
        for (const auto& table : analysis.jump_tables)
            jump_table_dispatches.insert(table.dispatch_address);
        const auto is_jump_table_dispatch = [&jump_table_dispatches](
                                                const std::uint32_t address) {
            return jump_table_dispatches.contains(address);
        };
        bool changed = false;
        for (const auto& copy : analysis.runtime_code_copies.copies) {
            const std::array origins{FunctionOrigin::RuntimeCopy};
            changed = add_seed(seeds,
                               copy.source_begin,
                               origins,
                               false,
                               ControlFlowEvidence::GuardedPartial) ||
                      changed;
            for (const auto& candidate : copy.patch_candidates) {
                changed = add_seed(seeds,
                                   candidate.target_address,
                                   origins,
                                   false,
                                   ControlFlowEvidence::GuardedPartial) ||
                          changed;
            }
        }
        for (const auto& continuation : analysis.static_return_continuations) {
            changed = add_seed(seeds,
                               continuation.target_address,
                               {},
                               false,
                               continuation.evidence) ||
                      changed;
        }
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
            report_progress("seed-expansion");
            continue;
        }

        std::vector<FunctionBoundary> function_boundaries;
        function_boundaries.reserve(
            analysis.recursive.functions.size());
        for (const auto& function : analysis.recursive.functions) {
            if (function.evidence != ControlFlowEvidence::Unresolved)
                function_boundaries.push_back(
                    {function.address, function.size});
        }
        bool boundary_contracts_active = false;
        bool function_values_stable = false;
        std::vector<
            std::pair<bool, std::vector<ResolvedControlFlowEdge>>>
            seen_function_value_contract_states;
        seen_function_value_contract_states.reserve(
            maximum_function_value_candidate_contract_iterations);
        while (!function_values_stable && !changed &&
               !analysis.function_budget_exhausted) {
            const auto provisional_edges = collect_function_value_edges(
                analysis.indirect_control_flow, analysis.jump_tables);
            const auto contract_state =
                std::pair{boundary_contracts_active, provisional_edges};
            if (std::find(
                    seen_function_value_contract_states.begin(),
                    seen_function_value_contract_states.end(),
                    contract_state) !=
                seen_function_value_contract_states.end()) {
                analysis.function_budget_exhausted = true;
                report_progress(
                    "function-values-candidate-contract-cycle-exhausted");
                break;
            }
            if (seen_function_value_contract_states.size() >=
                maximum_function_value_candidate_contract_iterations) {
                analysis.function_budget_exhausted = true;
                report_progress(
                    "function-values-candidate-contract-budget-exhausted");
                break;
            }
            seen_function_value_contract_states.push_back(contract_state);
            report_progress(
                seen_function_value_contract_states.size() == 1u
                    ? "function-values-start"
                    : "function-values-candidate-contract-reconcile");
            auto function_values =
                detail::analyze_function_values_with_guarded_entry_cache(
                    image,
                    analysis.recursive.instructions,
                    function_boundaries,
                    provisional_edges,
                    [&report_progress](
                        const FunctionValueAnalysisProgress& progress) {
                        std::string phase = "function-values-";
                        phase += progress.phase;
                        phase += "-f" +
                                 std::to_string(progress.functions);
                        phase += "-b" + std::to_string(progress.blocks);
                        phase +=
                            "-k" +
                            std::to_string(progress.fixpoint_iterations);
                        phase +=
                            "-d" +
                            std::to_string(progress.completed_functions);
                        phase += "-p" + std::to_string(progress.pending);
                        phase +=
                            "-r" +
                            std::to_string(progress.resolutions);
                        report_progress(phase);
                    },
                    guarded_native_entry_shapes);
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
                analysis.guarded_code_inventory_walk =
                    function_values.guarded_code_inventory
                        .walk_diagnostics;
                report_progress("function-values-budget-exhausted");
                break;
            }

            for (auto& proof : function_values.resolutions) {
                if (proof.targets.empty()) continue;
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
                const auto found =
                    resolution_by_address.find(proof.instruction_address);
                if (found == resolution_by_address.end()) continue;
                auto resolution =
                    analysis.indirect_control_flow.begin() +
                    static_cast<std::ptrdiff_t>(found->second);
                if (resolution->status == ResolutionStatus::Resolved ||
                    resolution->evidence ==
                        ControlFlowEvidence::ForcedOverride)
                    continue;
                if (control_flow_evidence_strength(proof.evidence) <
                    control_flow_evidence_strength(
                        resolution->evidence))
                    continue;
                resolution->status =
                    proof.guarded ? ResolutionStatus::Guarded
                                  : ResolutionStatus::Resolved;
                resolution->evidence =
                    proof.complete && !proof.guarded
                        ? ControlFlowEvidence::ProvenComplete
                        : proof.evidence;
                resolution->evidence_origins = {
                    AnalysisEvidenceOrigin::FunctionSummary};
                resolution->target =
                    proof.targets.size() == 1u
                        ? std::optional<std::uint32_t>(
                              proof.targets.front())
                        : std::nullopt;
                resolution->reason = std::move(proof.reason);
                resolution->targets = std::move(proof.targets);
                resolution->evidence_call_sites =
                    std::move(proof.call_sites);
                resolution->evidence_callees =
                    std::move(proof.callees);
            }
            if (boundary_contracts_active)
                static_cast<void>(apply_decode_boundary_downgrades());
            classify_dynamic_sites(analysis.recursive.instructions,
                                   analysis.indirect_control_flow);
            bind_partial_runtime_contracts(
                analysis.indirect_control_flow);

            // Resolution targets can safely grow the outer decode graph as
            // soon as their bounded proof exists. Inventory and diagnostics,
            // however, are published only from a relationally stable pass.
            for (const auto& resolution :
                 analysis.indirect_control_flow) {
                if (is_jump_table_dispatch(
                        resolution.instruction_address))
                    continue;
                changed =
                    add_resolution_seeds(seeds, resolution) || changed;
            }
            if (changed) break;

            // Once every proof target has had the opportunity to grow the
            // outer decode graph, fold boundary normalization into the same
            // candidate-contract snapshot as the FunctionValue
            // reconciliation.  Waiting until the unnormalized contracts had
            // first stabilized forced a whole additional FunctionValue pass
            // over an otherwise unchanged graph.
            if (!boundary_contracts_active) {
                boundary_contracts_active = true;
                static_cast<void>(
                    apply_decode_boundary_downgrades());
                classify_dynamic_sites(
                    analysis.recursive.instructions,
                    analysis.indirect_control_flow);
                bind_partial_runtime_contracts(
                    analysis.indirect_control_flow);
            }

            const auto reconciled_edges = collect_function_value_edges(
                analysis.indirect_control_flow, analysis.jump_tables);
            if (reconciled_edges != provisional_edges) continue;

            for (const auto& candidate :
                 function_values.guarded_code_inventory
                     .stored_code_addresses) {
                const std::array origins{
                    FunctionOrigin::StoredCodeAddress};
                changed =
                    add_seed(seeds,
                             candidate.target_address,
                             origins,
                             false,
                             ControlFlowEvidence::GuardedPartial) ||
                    changed;
            }
            for (const auto& table :
                 function_values.guarded_code_inventory
                     .returned_code_address_tables) {
                const std::array origins{
                    FunctionOrigin::GuardedSnapshot};
                for (const auto target : table.target_addresses) {
                    changed =
                        add_seed(seeds,
                                 target,
                                 origins,
                                 false,
                                 ControlFlowEvidence::GuardedPartial) ||
                        changed;
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
            auto guarded_code_inventory =
                std::move(function_values.guarded_code_inventory);
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
            analysis
                .guarded_code_inventory_candidate_budget_exhausted =
                guarded_code_inventory.candidate_budget_exhausted;
            analysis.guarded_code_inventory_walk =
                guarded_code_inventory.walk_diagnostics;
            analysis.guarded_code_shape_validation_work =
                guarded_code_inventory.shape_validation_work;
            analysis.guarded_code_shape_validation_work_budget =
                guarded_code_inventory.shape_validation_work_budget;
            analysis.guarded_code_shape_budget_exceeded_candidates =
                guarded_code_inventory
                    .shape_budget_exceeded_candidates;
            analysis.candidate_inventory_truncated =
                guarded_code_inventory.candidate_inventory_truncated;
            analysis.returned_table_scan_truncated =
                guarded_code_inventory.table_scan_truncated;
            final_guarded_code_inventory = guarded_code_inventory;
            analysis.function_value_summaries =
                std::move(function_values.summaries);
            function_values_stable = true;
            report_progress("function-values-complete");
        }
        if (analysis.function_budget_exhausted) {
            // Product export rejects both an internal summary-budget loss and
            // a candidate-contract closure that cannot reach a fixed point.
            break;
        }
        report_progress("summary-seed-expansion");
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
    report_progress("fixpoint-complete");
    const auto final_function_value_edges = collect_function_value_edges(
        analysis.indirect_control_flow, analysis.jump_tables);
    const auto final_boundary_change =
        apply_decode_boundary_downgrades();
    classify_dynamic_sites(analysis.recursive.instructions,
                           analysis.indirect_control_flow);
    bind_partial_runtime_contracts(analysis.indirect_control_flow);
    if (!analysis.function_budget_exhausted &&
        (final_boundary_change ||
         collect_function_value_edges(analysis.indirect_control_flow,
                                      analysis.jump_tables) !=
             final_function_value_edges)) {
        // The stable inner pass must already include the final boundary
        // normalization. Never publish an inventory produced from stale
        // candidate contracts if that invariant is violated.
        analysis.function_budget_exhausted = true;
        report_progress("function-values-boundary-contract-stale");
    }
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
    const auto final_blocks = build_basic_blocks(
        analysis.recursive.instructions,
        analysis.resolved_edges,
        final_block_leaders);
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
    std::sort(evidence_strings.begin(), evidence_strings.end());
    evidence_strings.erase(std::unique(evidence_strings.begin(), evidence_strings.end()),
                           evidence_strings.end());
    for (const auto& evidence : evidence_strings)
        static_cast<void>(analysis.evidence_ids.intern(evidence));
    analysis.jump_table_cache = jump_table_cache.counters();
    report_progress("complete");
    return analysis;
}

ControlFlowAnalysisResult analyze_control_flow(const katana::io::ExecutableImage& image,
                                               const AnalysisOverrides* overrides) {
    return analyze_control_flow(image, overrides, {});
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

} // namespace katana::analysis
