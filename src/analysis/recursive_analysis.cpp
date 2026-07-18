#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/code_address.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace katana::analysis {
namespace {

struct PendingAddress {
    std::uint32_t address = 0u;
    std::uint32_t incoming_address = 0u;
    std::optional<std::uint32_t> delay_slot_owner;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;

    [[nodiscard]] bool operator<(const PendingAddress& other) const noexcept {
        return std::tie(address, incoming_address, delay_slot_owner, evidence) <
               std::tie(other.address,
                        other.incoming_address,
                        other.delay_slot_owner,
                        other.evidence);
    }
};

void enqueue(std::deque<PendingAddress>& pending,
             std::set<PendingAddress>& scheduled,
             const std::uint32_t address,
             const std::uint32_t incoming_address,
             const std::optional<std::uint32_t> delay_slot_owner,
             const ControlFlowEvidence evidence) {
    PendingAddress work{address, incoming_address, delay_slot_owner, evidence};
    if (scheduled.insert(work).second) pending.push_back(std::move(work));
}

void enqueue_next(std::deque<PendingAddress>& pending,
                  std::set<PendingAddress>& scheduled,
                  const std::uint32_t address,
                  const std::uint32_t distance,
                  const ControlFlowEvidence evidence) {
    if (address <= std::numeric_limits<std::uint32_t>::max() - distance) {
        enqueue(pending, scheduled, address + distance, address, std::nullopt, evidence);
    }
}

void add_range(std::vector<ClassifiedRange>& ranges,
               const std::uint64_t start,
               const std::uint64_t end,
               const DiscoveredByteKind kind) {
    if (start >= end) {
        return;
    }
    if (!ranges.empty()) {
        auto& previous = ranges.back();
        const auto previous_end =
            static_cast<std::uint64_t>(previous.start_address) + previous.size;
        if (previous.kind == kind && previous_end == start) {
            previous.size += end - start;
            return;
        }
    }
    ranges.push_back({static_cast<std::uint32_t>(start), end - start, kind});
}

void classify_image(const katana::io::ExecutableImage& image,
                    const std::span<const katana::sh4::DisassemblyLine> discovered,
                    RecursiveAnalysisResult& result) {
    for (const auto& segment : image.segments()) {
        const auto segment_start = static_cast<std::uint64_t>(segment.virtual_address);
        const auto segment_end = segment.end_address();
        if (segment.kind == katana::io::SegmentKind::Data) {
            add_range(result.ranges, segment_start, segment_end, DiscoveredByteKind::Data);
            continue;
        }
        if (segment.kind != katana::io::SegmentKind::Code || !segment.permissions.executable) {
            add_range(result.ranges, segment_start, segment_end, DiscoveredByteKind::Unknown);
            continue;
        }

        auto cursor = segment_start;
        const auto committed_end = segment_start + segment.bytes.size();
        for (const auto& line : discovered) {
            const auto address = line.address;
            if (!segment.contains(address, 2u)) {
                continue;
            }
            const auto instruction_start = static_cast<std::uint64_t>(address);
            add_range(result.ranges, cursor, instruction_start, DiscoveredByteKind::Unknown);
            add_range(result.unreachable_code,
                      cursor,
                      std::min(instruction_start, committed_end),
                      DiscoveredByteKind::Unknown);
            add_range(
                result.ranges, instruction_start, instruction_start + 2u, DiscoveredByteKind::Code);
            cursor = instruction_start + 2u;
        }
        add_range(result.ranges, cursor, segment_end, DiscoveredByteKind::Unknown);
        add_range(result.unreachable_code, cursor, committed_end, DiscoveredByteKind::Unknown);
    }
}

void add_function_evidence(std::unordered_map<std::uint32_t, FunctionCandidate>& candidates,
                           const std::uint32_t address,
                           const FunctionOrigin origin,
                           const AnalysisConfidence confidence,
                           const ControlFlowEvidence evidence) {
    auto& candidate = candidates[address];
    candidate.address = address;
    if (static_cast<int>(confidence) > static_cast<int>(candidate.confidence)) {
        candidate.confidence = confidence;
    }
    if (control_flow_evidence_strength(evidence) >
        control_flow_evidence_strength(candidate.evidence))
        candidate.evidence = evidence;
    if (std::find(candidate.origins.begin(), candidate.origins.end(), origin) ==
        candidate.origins.end()) {
        candidate.origins.push_back(origin);
        std::sort(candidate.origins.begin(), candidate.origins.end());
    }
}

} // namespace

RecursiveAnalysisResult analyze_reachable_code(const katana::io::ExecutableImage& image,
                                               const RecursiveAnalysisOptions& options) {
    std::deque<PendingAddress> pending;
    std::set<PendingAddress> scheduled;
    std::unordered_set<std::uint32_t> delay_slots;
    std::unordered_map<std::uint32_t, katana::sh4::DisassemblyLine> discovered;
    std::unordered_map<std::uint32_t, FunctionCandidate> function_candidates;
    delay_slots.reserve(1024u);
    discovered.reserve(4096u);
    function_candidates.reserve(256u);

    for (const auto entry : image.entry_points()) {
        const auto validation = validate_committed_code_address(image, entry);
        if (!validation.valid()) {
            throw std::invalid_argument("Analyse-Einstiegspunkt ist ungueltig: " +
                                        std::string(code_address_status_name(validation.status)) +
                                        ".");
        }
        add_function_evidence(
            function_candidates,
            entry,
            FunctionOrigin::EntryPoint,
            AnalysisConfidence::Certain,
            ControlFlowEvidence::ProvenComplete);
        enqueue(pending,
                scheduled,
                entry,
                entry,
                std::nullopt,
                ControlFlowEvidence::ProvenComplete);
    }
    for (const auto& symbol : image.symbols()) {
        if (symbol.kind != katana::io::SymbolKind::Function || (symbol.address & 1u) != 0u ||
            !validate_committed_code_address(image, symbol.address).valid()) {
            continue;
        }
        enqueue(pending,
                scheduled,
                symbol.address,
                symbol.address,
                std::nullopt,
                ControlFlowEvidence::ProvenComplete);
        add_function_evidence(
            function_candidates,
            symbol.address,
            FunctionOrigin::Symbol,
            AnalysisConfidence::High,
            ControlFlowEvidence::ProvenComplete);
    }
    for (const auto& seed : options.additional_seeds) {
        if (!validate_committed_code_address(image, seed.address).valid()) {
            continue;
        }
        const auto evidence = seed.guarded_candidate &&
                                      seed.evidence == ControlFlowEvidence::ProvenComplete
                                  ? ControlFlowEvidence::GuardedPartial
                                  : seed.evidence;
        enqueue(pending, scheduled, seed.address, seed.address, std::nullopt, evidence);
        for (const auto origin : seed.function_origins) {
            const auto confidence =
                origin == FunctionOrigin::UserOverride      ? AnalysisConfidence::Certain
                : origin == FunctionOrigin::GuardedSnapshot ? AnalysisConfidence::Medium
                                                            : AnalysisConfidence::High;
            add_function_evidence(
                function_candidates, seed.address, origin, confidence, evidence);
        }
    }

    std::vector<AnalysisDiagnostic> diagnostics;
    std::vector<ContextualInstruction> result_contexts;

    while (!pending.empty()) {
        const auto work = pending.front();
        pending.pop_front();
        const auto address = work.address;
        const auto evidence = work.evidence;
        const auto validation = validate_decode_candidate(image, address);
        if (!validation.valid()) {
            continue;
        }
        const auto* segment = validation.segment;
        const auto offset = *segment->byte_offset(address);
        const auto opcode = katana::io::read_u16_le(segment->bytes, offset);

        katana::sh4::DisassemblyLine line;
        line.address = address;
        line.opcode = opcode;
        line.instruction = katana::sh4::decode(opcode);
        line.is_delay_slot = work.delay_slot_owner.has_value();
        line.target_address =
            katana::sh4::calculate_direct_branch_target(line.instruction, address);
        const auto [legacy, inserted] = discovered.emplace(address, line);
        if (!inserted && !line.is_delay_slot) legacy->second = line;

        ContextualInstruction contextual;
        contextual.line = line;
        contextual.incoming_address = work.incoming_address;
        contextual.delay_slot_owner = work.delay_slot_owner;
        contextual.evidence = evidence;
        result_contexts.push_back(std::move(contextual));

        if (!line.instruction.is_known()) {
            diagnostics.push_back({address, opcode, "unknown-opcode"});
            continue;
        }

        if (line.is_delay_slot) {
            delay_slots.insert(address);
            if (line.instruction.changes_control_flow())
                diagnostics.push_back({address, opcode, "control-flow-in-delay-slot"});
            continue;
        }

        const auto fallthrough_distance = line.instruction.has_delay_slot ? 4u : 2u;
        if (line.instruction.has_delay_slot) {
            if (address <= std::numeric_limits<std::uint32_t>::max() - 2u) {
                const auto delay_address = address + 2u;
                enqueue(pending, scheduled, delay_address, address, address, evidence);
                const auto delay_validation = validate_committed_code_address(image, delay_address);
                if (!delay_validation.valid()) {
                    diagnostics.push_back({address, opcode, "delay-slot-unavailable"});
                    continue;
                }
                const auto delay_offset = *delay_validation.segment->byte_offset(delay_address);
                const auto delay_opcode =
                    katana::io::read_u16_le(delay_validation.segment->bytes, delay_offset);
                if (!katana::sh4::decode(delay_opcode).is_known()) {
                    diagnostics.push_back({address, opcode, "delay-slot-unknown-opcode"});
                    continue;
                }
            }
        }

        switch (line.instruction.control_flow) {
        case katana::sh4::ControlFlowKind::None:
            enqueue_next(pending, scheduled, address, 2u, evidence);
            break;
        case katana::sh4::ControlFlowKind::ConditionalBranch:
            if (line.target_address.has_value()) {
                enqueue(pending,
                        scheduled,
                        *line.target_address,
                        address,
                        std::nullopt,
                        evidence);
            }
            enqueue_next(pending, scheduled, address, fallthrough_distance, evidence);
            break;
        case katana::sh4::ControlFlowKind::Call:
            if (line.target_address.has_value()) {
                enqueue(pending,
                        scheduled,
                        *line.target_address,
                        address,
                        std::nullopt,
                        evidence);
                if ((*line.target_address & 1u) == 0u &&
                    validate_committed_code_address(image, *line.target_address).valid()) {
                    add_function_evidence(function_candidates,
                                          *line.target_address,
                                          FunctionOrigin::DirectCall,
                                          AnalysisConfidence::High,
                                          evidence);
                }
            }
            enqueue_next(pending, scheduled, address, fallthrough_distance, evidence);
            break;
        case katana::sh4::ControlFlowKind::IndirectCall:
            enqueue_next(pending, scheduled, address, fallthrough_distance, evidence);
            break;
        case katana::sh4::ControlFlowKind::UnconditionalBranch:
            if (line.target_address.has_value()) {
                enqueue(pending,
                        scheduled,
                        *line.target_address,
                        address,
                        std::nullopt,
                        evidence);
            }
            break;
        case katana::sh4::ControlFlowKind::IndirectBranch:
        case katana::sh4::ControlFlowKind::Return:
        case katana::sh4::ControlFlowKind::Trap:
        case katana::sh4::ControlFlowKind::ExceptionReturn:
        case katana::sh4::ControlFlowKind::Halt:
            break;
        }
    }

    RecursiveAnalysisResult result;
    std::sort(result_contexts.begin(), result_contexts.end(), [](const auto& left,
                                                                 const auto& right) {
        return std::tie(left.line.address,
                        left.delay_slot_owner,
                        left.incoming_address,
                        left.evidence) <
               std::tie(right.line.address,
                        right.delay_slot_owner,
                        right.incoming_address,
                        right.evidence);
    });
    result.contextual_instructions = std::move(result_contexts);
    result.instructions.reserve(discovered.size());
    for (auto& [address, line] : discovered) {
        static_cast<void>(address);
        result.instructions.push_back(std::move(line));
    }
    std::sort(result.instructions.begin(),
              result.instructions.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    result.proven_instruction_addresses.reserve(result.instructions.size());
    result.guarded_candidate_instruction_addresses.reserve(result.instructions.size());
    std::set<std::uint32_t> proven_addresses;
    std::set<std::uint32_t> candidate_addresses;
    for (const auto& contextual : result.contextual_instructions) {
        if (control_flow_evidence_proven(contextual.evidence) &&
            contextual.line.instruction.is_known())
            proven_addresses.insert(contextual.line.address);
        else
            candidate_addresses.insert(contextual.line.address);
    }
    result.proven_instruction_addresses.assign(proven_addresses.begin(), proven_addresses.end());
    for (const auto address : candidate_addresses) {
        if (!proven_addresses.contains(address))
            result.guarded_candidate_instruction_addresses.push_back(address);
    }
    classify_image(image, result.instructions, result);
    result.diagnostics = std::move(diagnostics);
    std::sort(result.diagnostics.begin(),
              result.diagnostics.end(),
              [](const auto& left, const auto& right) {
                  if (left.address != right.address) {
                      return left.address < right.address;
                  }
                  if (left.opcode != right.opcode) {
                      return left.opcode < right.opcode;
                  }
                  return left.reason < right.reason;
              });
    result.diagnostics.erase(std::unique(result.diagnostics.begin(),
                                         result.diagnostics.end(),
                                         [](const auto& left, const auto& right) {
                                             return left.address == right.address &&
                                                    left.opcode == right.opcode &&
                                                    left.reason == right.reason;
                                         }),
                             result.diagnostics.end());
    result.functions.reserve(function_candidates.size());
    for (auto& [address, candidate] : function_candidates) {
        if (control_flow_evidence_proven(candidate.evidence) &&
            !proven_addresses.contains(address))
            candidate.evidence = ControlFlowEvidence::GuardedPartial;
        if (delay_slots.contains(address)) {
            result.conflicts.push_back(
                {address, 2u, AnalysisConflictKind::FunctionEntryInDelaySlot});
        }
        result.functions.push_back(std::move(candidate));
    }
    std::sort(result.functions.begin(),
              result.functions.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    std::sort(result.conflicts.begin(),
              result.conflicts.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    return result;
}

const char* discovered_byte_kind_name(const DiscoveredByteKind kind) noexcept {
    switch (kind) {
    case DiscoveredByteKind::Unknown:
        return "unknown";
    case DiscoveredByteKind::Code:
        return "code";
    case DiscoveredByteKind::Data:
        return "data";
    }
    return "unknown";
}

const char* function_origin_name(const FunctionOrigin origin) noexcept {
    switch (origin) {
    case FunctionOrigin::EntryPoint:
        return "entry-point";
    case FunctionOrigin::DirectCall:
        return "direct-call";
    case FunctionOrigin::IndirectCall:
        return "indirect-call";
    case FunctionOrigin::GuardedSnapshot:
        return "guarded-snapshot";
    case FunctionOrigin::JumpTableCall:
        return "jump-table-call";
    case FunctionOrigin::UserOverride:
        return "user-override";
    case FunctionOrigin::UserHint:
        return "user-hint";
    case FunctionOrigin::Symbol:
        return "symbol";
    }
    return "unknown";
}

const char* analysis_confidence_name(const AnalysisConfidence confidence) noexcept {
    switch (confidence) {
    case AnalysisConfidence::Low:
        return "low";
    case AnalysisConfidence::Medium:
        return "medium";
    case AnalysisConfidence::High:
        return "high";
    case AnalysisConfidence::Certain:
        return "certain";
    }
    return "unknown";
}

const char* analysis_conflict_kind_name(const AnalysisConflictKind kind) noexcept {
    switch (kind) {
    case AnalysisConflictKind::FunctionEntryInDelaySlot:
        return "function-entry-in-delay-slot";
    }
    return "unknown";
}

std::string format_recursive_analysis_report(const RecursiveAnalysisResult& result,
                                             const std::span<const SymbolicAddress> symbols) {
    std::ostringstream output;
    output << "Katana rekursive Analyse\n"
           << "Instruktionen: " << result.instructions.size() << '\n'
           << "Instruktionskontexte: " << result.contextual_instructions.size() << '\n'
           << "Funktionen: " << result.functions.size() << '\n'
           << "Bereiche: " << result.ranges.size() << '\n'
           << "Unerreichbar: " << result.unreachable_code.size() << '\n'
           << "Konflikte: " << result.conflicts.size() << '\n'
           << "Diagnosen: " << result.diagnostics.size() << "\n\n";
    const auto address = [&output](const std::uint32_t value) {
        output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value
               << std::dec << std::setfill(' ');
    };

    for (const auto& function : result.functions) {
        output << "Funktion ";
        address(function.address);
        if (const auto* symbol = find_symbolic_address(symbols, function.address)) {
            output << " Symbol=" << format_symbolic_address(*symbol);
        }
        output << " Konfidenz=" << analysis_confidence_name(function.confidence)
               << " Evidenz=" << control_flow_evidence_name(function.evidence) << " Herkunft=";
        for (std::size_t index = 0; index < function.origins.size(); ++index) {
            if (index != 0u) {
                output << ',';
            }
            output << function_origin_name(function.origins[index]);
        }
        output << '\n';
    }
    for (const auto& range : result.ranges) {
        output << "Bereich ";
        address(range.start_address);
        output << " Groesse=" << range.size << " Art=" << discovered_byte_kind_name(range.kind)
               << '\n';
    }
    for (const auto& range : result.unreachable_code) {
        output << "Unerreichbar ";
        address(range.start_address);
        output << " Groesse=" << range.size << '\n';
    }
    for (const auto& conflict : result.conflicts) {
        output << "Konflikt ";
        address(conflict.address);
        output << " Groesse=" << conflict.size
               << " Grund=" << analysis_conflict_kind_name(conflict.kind) << '\n';
    }
    for (const auto& diagnostic : result.diagnostics) {
        output << "Diagnose ";
        address(diagnostic.address);
        if (const auto* symbol = find_symbolic_address(symbols, diagnostic.address)) {
            output << " Symbol=" << format_symbolic_address(*symbol);
        }
        output << " Opcode=0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
               << diagnostic.opcode << std::dec << std::setfill(' ')
               << " Grund=" << diagnostic.reason << '\n';
    }
    return output.str();
}

} // namespace katana::analysis
