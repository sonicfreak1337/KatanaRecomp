#include "katana/analysis/recursive_analysis.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"

#include <cstdint>
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace katana::analysis {
namespace {

const katana::io::ImageSegment* executable_segment(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address
) {
    const auto* segment = image.find_segment(address, 2u);
    if (segment == nullptr || segment->kind != katana::io::SegmentKind::Code
        || !segment->permissions.executable) {
        return nullptr;
    }
    const auto offset = segment->byte_offset(address);
    if (!offset.has_value() || segment->bytes.size() < 2u
        || *offset > segment->bytes.size() - 2u) {
        return nullptr;
    }
    return segment;
}

void enqueue(
    std::deque<std::uint32_t>& pending,
    const std::uint32_t address
) {
    pending.push_back(address);
}

void enqueue_next(
    std::deque<std::uint32_t>& pending,
    const std::uint32_t address,
    const std::uint32_t distance
) {
    if (address <= std::numeric_limits<std::uint32_t>::max() - distance) {
        enqueue(pending, address + distance);
    }
}

void add_range(
    std::vector<ClassifiedRange>& ranges,
    const std::uint64_t start,
    const std::uint64_t end,
    const DiscoveredByteKind kind
) {
    if (start >= end) {
        return;
    }
    if (!ranges.empty()) {
        auto& previous = ranges.back();
        const auto previous_end = static_cast<std::uint64_t>(previous.start_address) + previous.size;
        if (previous.kind == kind && previous_end == start) {
            previous.size += end - start;
            return;
        }
    }
    ranges.push_back({static_cast<std::uint32_t>(start), end - start, kind});
}

void classify_image(
    const katana::io::ExecutableImage& image,
    const std::map<std::uint32_t, katana::sh4::DisassemblyLine>& discovered,
    RecursiveAnalysisResult& result
) {
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
        for (const auto& [address, line] : discovered) {
            static_cast<void>(line);
            if (!segment.contains(address, 2u)) {
                continue;
            }
            const auto instruction_start = static_cast<std::uint64_t>(address);
            add_range(result.ranges, cursor, instruction_start, DiscoveredByteKind::Unknown);
            add_range(
                result.unreachable_code,
                cursor,
                std::min(instruction_start, committed_end),
                DiscoveredByteKind::Unknown
            );
            add_range(result.ranges, instruction_start, instruction_start + 2u, DiscoveredByteKind::Code);
            cursor = instruction_start + 2u;
        }
        add_range(result.ranges, cursor, segment_end, DiscoveredByteKind::Unknown);
        add_range(
            result.unreachable_code,
            cursor,
            committed_end,
            DiscoveredByteKind::Unknown
        );
    }
}

void add_function_evidence(
    std::map<std::uint32_t, FunctionCandidate>& candidates,
    const std::uint32_t address,
    const FunctionOrigin origin,
    const AnalysisConfidence confidence
) {
    auto& candidate = candidates[address];
    candidate.address = address;
    if (static_cast<int>(confidence) > static_cast<int>(candidate.confidence)) {
        candidate.confidence = confidence;
    }
    if (std::find(candidate.origins.begin(), candidate.origins.end(), origin) == candidate.origins.end()) {
        candidate.origins.push_back(origin);
        std::sort(candidate.origins.begin(), candidate.origins.end());
    }
}

}

RecursiveAnalysisResult analyze_reachable_code(const katana::io::ExecutableImage& image) {
    std::deque<std::uint32_t> pending(image.entry_points().begin(), image.entry_points().end());
    std::set<std::uint32_t> processed;
    std::set<std::uint32_t> delay_slots;
    std::map<std::uint32_t, katana::sh4::DisassemblyLine> discovered;
    std::map<std::uint32_t, FunctionCandidate> function_candidates;

    for (const auto entry : image.entry_points()) {
        if ((entry & 1u) != 0u || executable_segment(image, entry) == nullptr) {
            throw std::invalid_argument("Analyse-Einstiegspunkt liegt nicht in committed ausfuehrbarem Code.");
        }
        add_function_evidence(
            function_candidates,
            entry,
            FunctionOrigin::EntryPoint,
            AnalysisConfidence::Certain
        );
    }
    for (const auto& symbol : image.symbols()) {
        if (symbol.kind != katana::io::SymbolKind::Function || (symbol.address & 1u) != 0u
            || executable_segment(image, symbol.address) == nullptr) {
            continue;
        }
        enqueue(pending, symbol.address);
        add_function_evidence(
            function_candidates,
            symbol.address,
            FunctionOrigin::Symbol,
            AnalysisConfidence::High
        );
    }

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        if (!processed.insert(address).second) {
            continue;
        }

        const auto* segment = executable_segment(image, address);
        if (segment == nullptr || (address & 1u) != 0u) {
            continue;
        }
        const auto offset = *segment->byte_offset(address);
        const auto opcode = katana::io::read_u16_le(segment->bytes, offset);

        katana::sh4::DisassemblyLine line;
        line.address = address;
        line.opcode = opcode;
        line.instruction = katana::sh4::decode(opcode);
        line.is_delay_slot = delay_slots.contains(address);
        line.target_address = katana::sh4::calculate_direct_branch_target(line.instruction, address);
        discovered.emplace(address, line);

        if (line.is_delay_slot) {
            continue;
        }

        const auto fallthrough_distance = line.instruction.has_delay_slot ? 4u : 2u;
        if (line.instruction.has_delay_slot) {
            if (address <= std::numeric_limits<std::uint32_t>::max() - 2u) {
                const auto delay_address = address + 2u;
                delay_slots.insert(delay_address);
                if (const auto iterator = discovered.find(delay_address); iterator != discovered.end()) {
                    iterator->second.is_delay_slot = true;
                }
                enqueue(pending, delay_address);
            }
        }

        switch (line.instruction.control_flow) {
            case katana::sh4::ControlFlowKind::None:
                enqueue_next(pending, address, 2u);
                break;
            case katana::sh4::ControlFlowKind::ConditionalBranch:
                if (line.target_address.has_value()) {
                    enqueue(pending, *line.target_address);
                }
                enqueue_next(pending, address, fallthrough_distance);
                break;
            case katana::sh4::ControlFlowKind::Call:
                if (line.target_address.has_value()) {
                    enqueue(pending, *line.target_address);
                    if ((*line.target_address & 1u) == 0u
                        && executable_segment(image, *line.target_address) != nullptr) {
                        add_function_evidence(
                            function_candidates,
                            *line.target_address,
                            FunctionOrigin::DirectCall,
                            AnalysisConfidence::High
                        );
                    }
                }
                enqueue_next(pending, address, fallthrough_distance);
                break;
            case katana::sh4::ControlFlowKind::IndirectCall:
                enqueue_next(pending, address, fallthrough_distance);
                break;
            case katana::sh4::ControlFlowKind::UnconditionalBranch:
                if (line.target_address.has_value()) {
                    enqueue(pending, *line.target_address);
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
    classify_image(image, discovered, result);
    result.functions.reserve(function_candidates.size());
    for (auto& [address, candidate] : function_candidates) {
        if (delay_slots.contains(address)) {
            result.conflicts.push_back({
                address,
                2u,
                AnalysisConflictKind::FunctionEntryInDelaySlot
            });
        }
        result.functions.push_back(std::move(candidate));
    }
    return result;
}

const char* discovered_byte_kind_name(const DiscoveredByteKind kind) noexcept {
    switch (kind) {
        case DiscoveredByteKind::Unknown: return "unknown";
        case DiscoveredByteKind::Code: return "code";
        case DiscoveredByteKind::Data: return "data";
    }
    return "unknown";
}

const char* function_origin_name(const FunctionOrigin origin) noexcept {
    switch (origin) {
        case FunctionOrigin::EntryPoint: return "entry-point";
        case FunctionOrigin::DirectCall: return "direct-call";
        case FunctionOrigin::Symbol: return "symbol";
    }
    return "unknown";
}

const char* analysis_confidence_name(const AnalysisConfidence confidence) noexcept {
    switch (confidence) {
        case AnalysisConfidence::Low: return "low";
        case AnalysisConfidence::Medium: return "medium";
        case AnalysisConfidence::High: return "high";
        case AnalysisConfidence::Certain: return "certain";
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

}
