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
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace katana::analysis {
namespace {

void enqueue(std::deque<std::uint32_t>& pending,
             std::unordered_set<std::uint32_t>& scheduled,
             const std::uint32_t address) {
    if (scheduled.insert(address).second) {
        pending.push_back(address);
    }
}

void enqueue_next(std::deque<std::uint32_t>& pending,
                  std::unordered_set<std::uint32_t>& scheduled,
                  const std::uint32_t address,
                  const std::uint32_t distance) {
    if (address <= std::numeric_limits<std::uint32_t>::max() - distance) {
        enqueue(pending, scheduled, address + distance);
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
                           const AnalysisConfidence confidence) {
    auto& candidate = candidates[address];
    candidate.address = address;
    if (static_cast<int>(confidence) > static_cast<int>(candidate.confidence)) {
        candidate.confidence = confidence;
    }
    if (std::find(candidate.origins.begin(), candidate.origins.end(), origin) ==
        candidate.origins.end()) {
        candidate.origins.push_back(origin);
        std::sort(candidate.origins.begin(), candidate.origins.end());
    }
}

} // namespace

RecursiveAnalysisResult analyze_reachable_code(const katana::io::ExecutableImage& image,
                                               const RecursiveAnalysisOptions& options) {
    std::deque<std::uint32_t> pending;
    std::unordered_set<std::uint32_t> scheduled;
    std::unordered_set<std::uint32_t> delay_slots;
    std::unordered_map<std::uint32_t, katana::sh4::DisassemblyLine> discovered;
    std::unordered_map<std::uint32_t, FunctionCandidate> function_candidates;
    scheduled.reserve(4096u);
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
            function_candidates, entry, FunctionOrigin::EntryPoint, AnalysisConfidence::Certain);
        enqueue(pending, scheduled, entry);
    }
    for (const auto& symbol : image.symbols()) {
        if (symbol.kind != katana::io::SymbolKind::Function || (symbol.address & 1u) != 0u ||
            !validate_committed_code_address(image, symbol.address).valid()) {
            continue;
        }
        enqueue(pending, scheduled, symbol.address);
        add_function_evidence(
            function_candidates, symbol.address, FunctionOrigin::Symbol, AnalysisConfidence::High);
    }
    for (const auto& seed : options.additional_seeds) {
        if (!validate_committed_code_address(image, seed.address).valid()) {
            continue;
        }
        enqueue(pending, scheduled, seed.address);
        for (const auto origin : seed.function_origins) {
            const auto confidence =
                origin == FunctionOrigin::UserOverride      ? AnalysisConfidence::Certain
                : origin == FunctionOrigin::GuardedSnapshot ? AnalysisConfidence::Medium
                                                            : AnalysisConfidence::High;
            add_function_evidence(function_candidates, seed.address, origin, confidence);
        }
    }

    std::vector<AnalysisDiagnostic> diagnostics;

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        const auto validation = validate_committed_code_address(image, address);
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
        line.is_delay_slot = delay_slots.contains(address);
        line.target_address =
            katana::sh4::calculate_direct_branch_target(line.instruction, address);
        discovered.emplace(address, line);

        if (!line.instruction.is_known()) {
            diagnostics.push_back({address, opcode, "unknown-opcode"});
            continue;
        }

        if (line.is_delay_slot) {
            continue;
        }

        const auto fallthrough_distance = line.instruction.has_delay_slot ? 4u : 2u;
        if (line.instruction.has_delay_slot) {
            if (address <= std::numeric_limits<std::uint32_t>::max() - 2u) {
                const auto delay_address = address + 2u;
                delay_slots.insert(delay_address);
                if (const auto iterator = discovered.find(delay_address);
                    iterator != discovered.end()) {
                    iterator->second.is_delay_slot = true;
                }
                enqueue(pending, scheduled, delay_address);
                const auto delay_validation = validate_committed_code_address(image, delay_address);
                if (!delay_validation.valid()) {
                    continue;
                }
                const auto delay_offset = *delay_validation.segment->byte_offset(delay_address);
                const auto delay_opcode =
                    katana::io::read_u16_le(delay_validation.segment->bytes, delay_offset);
                if (!katana::sh4::decode(delay_opcode).is_known()) {
                    continue;
                }
            }
        }

        switch (line.instruction.control_flow) {
        case katana::sh4::ControlFlowKind::None:
            enqueue_next(pending, scheduled, address, 2u);
            break;
        case katana::sh4::ControlFlowKind::ConditionalBranch:
            if (line.target_address.has_value()) {
                enqueue(pending, scheduled, *line.target_address);
            }
            enqueue_next(pending, scheduled, address, fallthrough_distance);
            break;
        case katana::sh4::ControlFlowKind::Call:
            if (line.target_address.has_value()) {
                enqueue(pending, scheduled, *line.target_address);
                if ((*line.target_address & 1u) == 0u &&
                    validate_committed_code_address(image, *line.target_address).valid()) {
                    add_function_evidence(function_candidates,
                                          *line.target_address,
                                          FunctionOrigin::DirectCall,
                                          AnalysisConfidence::High);
                }
            }
            enqueue_next(pending, scheduled, address, fallthrough_distance);
            break;
        case katana::sh4::ControlFlowKind::IndirectCall:
            enqueue_next(pending, scheduled, address, fallthrough_distance);
            break;
        case katana::sh4::ControlFlowKind::UnconditionalBranch:
            if (line.target_address.has_value()) {
                enqueue(pending, scheduled, *line.target_address);
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
    result.instructions.reserve(discovered.size());
    for (auto& [address, line] : discovered) {
        static_cast<void>(address);
        result.instructions.push_back(std::move(line));
    }
    std::sort(result.instructions.begin(),
              result.instructions.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
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
    result.functions.reserve(function_candidates.size());
    for (auto& [address, candidate] : function_candidates) {
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
        output << " Konfidenz=" << analysis_confidence_name(function.confidence) << " Herkunft=";
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
