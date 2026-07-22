#include "katana/analysis/executable_inventory.hpp"

#include "katana/io/json_report.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace katana::analysis {
namespace {

template <typename T> constexpr auto index(const T value) noexcept {
    return static_cast<std::size_t>(value);
}

PrecompileClass precompile_class(const ExecutableByteClass value,
                                 const RangeProofClass proof) noexcept {
    switch (value) {
    case ExecutableByteClass::ProvenReachableCode:
    case ExecutableByteClass::RuntimeDiscoveredCode:
        return PrecompileClass::InitiallyReachable;
    case ExecutableByteClass::UnreachableDecodableCode:
        return PrecompileClass::StaticallyDiscoverable;
    case ExecutableByteClass::ModuleCandidate:
        return PrecompileClass::LoadableModule;
    case ExecutableByteClass::OverlayCandidate:
        return PrecompileClass::RuntimeMaterializable;
    case ExecutableByteClass::EmbeddedData:
    case ExecutableByteClass::LiteralPool:
    case ExecutableByteClass::JumpTable:
    case ExecutableByteClass::PointerTable:
        return PrecompileClass::NeverExecutedData;
    case ExecutableByteClass::Padding:
        return proof == RangeProofClass::Proven ? PrecompileClass::NeverExecutedData
                                                : PrecompileClass::Unknown;
    case ExecutableByteClass::CompressedOrEncoded:
    case ExecutableByteClass::UnknownExecutable:
    case ExecutableByteClass::Count:
        return PrecompileClass::Unknown;
    }
    return PrecompileClass::Unknown;
}

MixedRangeRole range_role(const ExecutableByteClass value, const bool writable) noexcept {
    switch (value) {
    case ExecutableByteClass::ProvenReachableCode:
        return MixedRangeRole::ProvenCode;
    case ExecutableByteClass::RuntimeDiscoveredCode:
    case ExecutableByteClass::UnreachableDecodableCode:
        return MixedRangeRole::ReachableCode;
    case ExecutableByteClass::OverlayCandidate:
        return MixedRangeRole::RuntimeMaterializableCode;
    case ExecutableByteClass::LiteralPool:
        return MixedRangeRole::LiteralPool;
    case ExecutableByteClass::JumpTable:
        return MixedRangeRole::JumpTable;
    case ExecutableByteClass::PointerTable:
        return MixedRangeRole::PointerTable;
    case ExecutableByteClass::EmbeddedData:
        return writable ? MixedRangeRole::WritableData : MixedRangeRole::ReadOnlyData;
    case ExecutableByteClass::Padding:
        return MixedRangeRole::Padding;
    case ExecutableByteClass::CompressedOrEncoded:
        return MixedRangeRole::CompressedOrEncoded;
    case ExecutableByteClass::ModuleCandidate:
        return MixedRangeRole::ModulePayload;
    case ExecutableByteClass::UnknownExecutable:
    case ExecutableByteClass::Count:
        return MixedRangeRole::Unknown;
    }
    return MixedRangeRole::Unknown;
}

RangeProofClass default_proof(const ExecutableByteClass value) noexcept {
    switch (value) {
    case ExecutableByteClass::ProvenReachableCode:
    case ExecutableByteClass::EmbeddedData:
    case ExecutableByteClass::LiteralPool:
    case ExecutableByteClass::JumpTable:
    case ExecutableByteClass::PointerTable:
        return RangeProofClass::Proven;
    case ExecutableByteClass::RuntimeDiscoveredCode:
    case ExecutableByteClass::UnreachableDecodableCode:
        return RangeProofClass::Guarded;
    case ExecutableByteClass::Padding:
    case ExecutableByteClass::OverlayCandidate:
    case ExecutableByteClass::ModuleCandidate:
    case ExecutableByteClass::CompressedOrEncoded:
        return RangeProofClass::Candidate;
    case ExecutableByteClass::UnknownExecutable:
    case ExecutableByteClass::Count:
        return RangeProofClass::Unknown;
    }
    return RangeProofClass::Unknown;
}

const char* classification_reason(const ExecutableByteClass value,
                                  const RangeProofClass proof) noexcept {
    switch (value) {
    case ExecutableByteClass::ProvenReachableCode:
        return "instruction reached by proven control flow";
    case ExecutableByteClass::RuntimeDiscoveredCode:
        return "instruction reached by guarded control-flow evidence";
    case ExecutableByteClass::UnreachableDecodableCode:
        return "known instruction corroborated by a control-flow target";
    case ExecutableByteClass::EmbeddedData:
        return "object symbol bounds prove a data range";
    case ExecutableByteClass::LiteralPool:
        return "PC-relative load references this literal";
    case ExecutableByteClass::JumpTable:
        return "resolved jump-table entry";
    case ExecutableByteClass::PointerTable:
        return "referenced aligned table with classified executable targets";
    case ExecutableByteClass::Padding:
        return proof == RangeProofClass::Proven
                   ? "aligned trailing fill with no code, relocation, module, or reference evidence"
                   : "repeated fill candidate lacks a complete exclusion proof";
    case ExecutableByteClass::OverlayCandidate:
        return "overlay load phase has explicit source provenance";
    case ExecutableByteClass::ModuleCandidate:
        return "runtime module load phase has explicit source provenance";
    case ExecutableByteClass::CompressedOrEncoded:
        return "high-entropy candidate; execution role remains unproven";
    case ExecutableByteClass::UnknownExecutable:
    case ExecutableByteClass::Count:
        return "no sufficient code, data, padding, or module proof";
    }
    return "no sufficient proof";
}

struct SegmentInventory {
    const io::ImageSegment* segment = nullptr;
    std::size_t segment_index = 0u;
    std::vector<ExecutableByteClass> bytes;
    std::vector<RangeProofClass> proofs;
};

void mark_range(SegmentInventory& inventory,
                const std::uint32_t address,
                const std::size_t width,
                const ExecutableByteClass byte_class,
                const RangeProofClass proof) {
    if (inventory.segment == nullptr || width == 0u || address < inventory.segment->virtual_address)
        return;
    const auto offset = static_cast<std::uint64_t>(address) - inventory.segment->virtual_address;
    if (offset > inventory.bytes.size() || width > inventory.bytes.size() - offset) return;
    std::fill_n(inventory.bytes.begin() + static_cast<std::ptrdiff_t>(offset), width, byte_class);
    std::fill_n(inventory.proofs.begin() + static_cast<std::ptrdiff_t>(offset), width, proof);
}

SegmentInventory* find_segment(std::vector<SegmentInventory>& segments,
                               const std::uint32_t address,
                               const std::size_t width) {
    const auto found = std::find_if(segments.begin(), segments.end(), [&](const auto& candidate) {
        return candidate.segment->contains(address, width) &&
               candidate.segment->byte_offset(address).has_value() &&
               width <= candidate.segment->bytes.size() - *candidate.segment->byte_offset(address);
    });
    return found == segments.end() ? nullptr : &*found;
}

std::uint64_t count_between(const std::set<std::uint32_t>& values,
                            const std::uint32_t begin,
                            const std::uint64_t size) {
    const auto end64 = static_cast<std::uint64_t>(begin) + size;
    const auto end = end64 > std::numeric_limits<std::uint32_t>::max()
                         ? std::numeric_limits<std::uint32_t>::max()
                         : static_cast<std::uint32_t>(end64);
    return static_cast<std::uint64_t>(
        std::distance(values.lower_bound(begin), values.lower_bound(end)));
}

bool intersects(const std::set<std::uint32_t>& values,
                const std::uint32_t begin,
                const std::uint64_t size) {
    return count_between(values, begin, size) != 0u;
}

std::uint32_t read_u32(const io::ImageSegment& segment, const std::size_t offset) {
    return static_cast<std::uint32_t>(segment.bytes[offset]) |
           (static_cast<std::uint32_t>(segment.bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(segment.bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(segment.bytes[offset + 3u]) << 24u);
}

std::uint32_t entropy_millibits(const io::ImageSegment& segment,
                                const std::size_t begin,
                                const std::size_t size) {
    if (size == 0u) return 0u;
    std::array<std::size_t, 256u> histogram{};
    for (std::size_t offset = begin; offset < begin + size; ++offset)
        ++histogram[segment.bytes[offset]];
    double entropy = 0.0;
    for (const auto count : histogram) {
        if (count == 0u) continue;
        const auto probability = static_cast<double>(count) / static_cast<double>(size);
        entropy -= probability * std::log2(probability);
    }
    return static_cast<std::uint32_t>(std::llround(entropy * 1000.0));
}

std::uint32_t decode_density_ppm(const io::ImageSegment& segment,
                                 const std::size_t begin,
                                 const std::size_t size) {
    const auto pairs = size / 2u;
    if (pairs == 0u) return 0u;
    std::size_t known = 0u;
    for (std::size_t offset = begin; offset + 1u < begin + size; offset += 2u) {
        const auto opcode = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(segment.bytes[offset]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(segment.bytes[offset + 1u])
                                       << 8u));
        if (sh4::decode(opcode).is_known()) ++known;
    }
    return static_cast<std::uint32_t>((known * 1'000'000ull) / pairs);
}

std::uint64_t distance_to_code(const std::set<std::uint32_t>& code, const std::uint32_t address) {
    if (code.empty()) return std::numeric_limits<std::uint64_t>::max();
    const auto next = code.lower_bound(address);
    auto distance = std::numeric_limits<std::uint64_t>::max();
    if (next != code.end()) distance = *next - static_cast<std::uint64_t>(address);
    if (next != code.begin()) {
        const auto previous = *std::prev(next);
        distance = std::min(distance, static_cast<std::uint64_t>(address) - previous);
    }
    return distance;
}

} // namespace

ExecutableByteInventory build_executable_byte_inventory(const io::ExecutableImage& image,
                                                        const ControlFlowAnalysisResult& analysis) {
    std::vector<SegmentInventory> segments;
    for (std::size_t segment_index = 0u; segment_index < image.segments().size(); ++segment_index) {
        const auto& segment = image.segments()[segment_index];
        if (!segment.permissions.executable || segment.bytes.empty()) continue;
        auto initial = ExecutableByteClass::UnknownExecutable;
        if (segment.load_phase == io::ImageLoadPhase::RuntimeModule)
            initial = ExecutableByteClass::ModuleCandidate;
        else if (segment.load_phase == io::ImageLoadPhase::Overlay)
            initial = ExecutableByteClass::OverlayCandidate;
        segments.push_back(
            {&segment,
             segment_index,
             std::vector<ExecutableByteClass>(segment.bytes.size(), initial),
             std::vector<RangeProofClass>(segment.bytes.size(), default_proof(initial))});
    }

    std::set<std::uint32_t> static_references;
    std::set<std::uint32_t> relocations;
    std::set<std::uint32_t> potential_targets(image.entry_points().begin(),
                                              image.entry_points().end());
    std::set<std::uint32_t> proven_code(analysis.recursive.proven_instruction_addresses.begin(),
                                        analysis.recursive.proven_instruction_addresses.end());
    potential_targets.insert(analysis.recursive.proven_instruction_addresses.begin(),
                             analysis.recursive.proven_instruction_addresses.end());
    potential_targets.insert(analysis.recursive.guarded_candidate_instruction_addresses.begin(),
                             analysis.recursive.guarded_candidate_instruction_addresses.end());
    for (const auto& function : analysis.recursive.functions)
        potential_targets.insert(function.address);
    for (const auto& edge : analysis.resolved_edges)
        potential_targets.insert(edge.target_address);
    for (const auto& site : analysis.sites)
        potential_targets.insert(site.targets.begin(), site.targets.end());
    for (const auto& table : analysis.jump_tables)
        for (const auto& entry : table.entries)
            if (entry.accepted) potential_targets.insert(entry.target);
    for (const auto& relocation : image.relocations()) {
        relocations.insert(relocation.address);
        if (relocation.applied_value.has_value())
            potential_targets.insert(*relocation.applied_value);
        if (relocation.symbol_address != 0u) potential_targets.insert(relocation.symbol_address);
    }
    for (const auto& symbol : image.symbols())
        if (symbol.kind == io::SymbolKind::Function) potential_targets.insert(symbol.address);

    for (auto& inventory : segments) {
        const auto& bytes = inventory.segment->bytes;
        std::size_t cursor = 0u;
        while (cursor < bytes.size()) {
            if (bytes[cursor] != 0x00u && bytes[cursor] != 0xffu) {
                ++cursor;
                continue;
            }
            const auto value = bytes[cursor];
            const auto begin = cursor;
            while (cursor < bytes.size() && bytes[cursor] == value)
                ++cursor;
            if (cursor - begin >= 16u)
                mark_range(inventory,
                           inventory.segment->virtual_address + static_cast<std::uint32_t>(begin),
                           cursor - begin,
                           ExecutableByteClass::Padding,
                           RangeProofClass::Candidate);
        }
    }

    for (const auto& symbol : image.symbols()) {
        if (symbol.kind != io::SymbolKind::Object || symbol.size == 0u) continue;
        if (auto* segment = find_segment(segments, symbol.address, symbol.size); segment != nullptr)
            mark_range(*segment,
                       symbol.address,
                       symbol.size,
                       ExecutableByteClass::EmbeddedData,
                       RangeProofClass::Proven);
    }

    for (const auto& table : analysis.jump_tables) {
        const auto width = table.encoding == JumpTableEncoding::Absolute32 ? 4u : 2u;
        for (const auto& entry : table.entries) {
            if (!entry.accepted) continue;
            static_references.insert(entry.entry_address);
            if (auto* segment = find_segment(segments, entry.entry_address, width);
                segment != nullptr)
                mark_range(*segment,
                           entry.entry_address,
                           width,
                           ExecutableByteClass::JumpTable,
                           table.aot_candidates_only ? RangeProofClass::Candidate
                                                     : RangeProofClass::Proven);
        }
    }

    std::set<std::uint32_t> pointer_table_seeds;
    for (const auto& line : analysis.recursive.instructions) {
        const auto kind = line.instruction.kind;
        std::uint64_t target = 0u;
        std::size_t width = 0u;
        if (kind == sh4::InstructionKind::MovWordLoadPcRelative) {
            target = static_cast<std::uint64_t>(line.address) + 4u +
                     static_cast<std::uint32_t>(line.instruction.displacement);
            width = 2u;
        } else if (kind == sh4::InstructionKind::MovLongLoadPcRelative) {
            target = ((static_cast<std::uint64_t>(line.address) + 4u) & ~std::uint64_t{3u}) +
                     static_cast<std::uint32_t>(line.instruction.displacement);
            width = 4u;
        } else if (kind == sh4::InstructionKind::MoveAddressPcRelative) {
            target = ((static_cast<std::uint64_t>(line.address) + 4u) & ~std::uint64_t{3u}) +
                     static_cast<std::uint32_t>(line.instruction.displacement);
            if (target <= std::numeric_limits<std::uint32_t>::max()) {
                static_references.insert(static_cast<std::uint32_t>(target));
                pointer_table_seeds.insert(static_cast<std::uint32_t>(target));
            }
            continue;
        }
        if (width != 0u && target <= std::numeric_limits<std::uint32_t>::max()) {
            const auto address = static_cast<std::uint32_t>(target);
            static_references.insert(address);
            if (auto* segment = find_segment(segments, address, width); segment != nullptr)
                mark_range(*segment,
                           address,
                           width,
                           ExecutableByteClass::LiteralPool,
                           RangeProofClass::Proven);
        }
        if (const auto direct = sh4::calculate_direct_branch_target(line.instruction, line.address);
            direct.has_value())
            potential_targets.insert(*direct);
    }

    for (auto& inventory : segments) {
        for (const auto seed : pointer_table_seeds) {
            const auto offset = inventory.segment->byte_offset(seed);
            if (!offset.has_value() || (seed & 3u) != 0u) continue;
            std::size_t entries = 0u;
            while (*offset + (entries + 1u) * 4u <= inventory.segment->bytes.size() &&
                   entries < 256u) {
                const auto target = read_u32(*inventory.segment, *offset + entries * 4u);
                if (!potential_targets.contains(target) && !proven_code.contains(target)) break;
                ++entries;
            }
            if (entries >= 3u)
                mark_range(inventory,
                           seed,
                           entries * 4u,
                           ExecutableByteClass::PointerTable,
                           RangeProofClass::Proven);
        }
    }

    const std::set<std::uint32_t> guarded(
        analysis.recursive.guarded_candidate_instruction_addresses.begin(),
        analysis.recursive.guarded_candidate_instruction_addresses.end());
    for (const auto& line : analysis.recursive.instructions) {
        if (auto* segment = find_segment(segments, line.address, 2u); segment != nullptr)
            mark_range(*segment,
                       line.address,
                       2u,
                       guarded.contains(line.address) ? ExecutableByteClass::RuntimeDiscoveredCode
                                                      : ExecutableByteClass::ProvenReachableCode,
                       guarded.contains(line.address) ? RangeProofClass::Guarded
                                                      : RangeProofClass::Proven);
    }

    // Decode is only supporting evidence here: an unknown byte pair becomes a code candidate
    // only when a separate entry/edge/relocation/function contract points at it.
    for (const auto target : potential_targets) {
        auto* segment = find_segment(segments, target, 2u);
        if (segment == nullptr) continue;
        const auto offset = *segment->segment->byte_offset(target);
        if (segment->bytes[offset] != ExecutableByteClass::UnknownExecutable &&
            segment->bytes[offset] != ExecutableByteClass::Padding)
            continue;
        const auto opcode = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(segment->segment->bytes[offset]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(segment->segment->bytes[offset + 1u]) << 8u));
        if (sh4::decode(opcode).is_known())
            mark_range(*segment,
                       target,
                       2u,
                       ExecutableByteClass::UnreachableDecodableCode,
                       RangeProofClass::Guarded);
    }

    // Only a source-bound mixed boot image may prove trailing fill. Interior fill and fill in a
    // generic raw code segment stay blocking candidates.
    for (auto& inventory : segments) {
        const auto& segment = *inventory.segment;
        if (segment.kind != io::SegmentKind::Mixed ||
            segment.source_kind != io::ImageSourceKind::DiscBootFile ||
            segment.load_phase != io::ImageLoadPhase::Initial)
            continue;
        auto begin = inventory.bytes.size();
        while (begin > 0u && inventory.bytes[begin - 1u] == ExecutableByteClass::Padding)
            --begin;
        if (begin == inventory.bytes.size() || (begin & 31u) != 0u) continue;
        const auto address = segment.virtual_address + static_cast<std::uint32_t>(begin);
        const auto size = inventory.bytes.size() - begin;
        if (!intersects(static_references, address, size) &&
            !intersects(relocations, address, size) &&
            !intersects(potential_targets, address, size) &&
            !intersects(proven_code, address, size))
            std::fill(inventory.proofs.begin() + static_cast<std::ptrdiff_t>(begin),
                      inventory.proofs.end(),
                      RangeProofClass::Proven);
    }

    ExecutableByteInventory result;
    for (const auto& inventory : segments) {
        const auto& source = *inventory.segment;
        ExecutableInventoryGroup group;
        group.segment_index = inventory.segment_index;
        group.source_kind = source.source_kind;
        group.load_phase = source.load_phase;
        group.writable = source.permissions.writable;
        group.committed_bytes = source.bytes.size();
        group.zero_fill_bytes = source.memory_size - source.bytes.size();
        result.committed_executable_bytes += inventory.bytes.size();

        std::size_t cursor = 0u;
        while (cursor < inventory.bytes.size()) {
            const auto byte_class = inventory.bytes[cursor];
            const auto proof = inventory.proofs[cursor];
            const auto begin = cursor;
            while (cursor < inventory.bytes.size() && inventory.bytes[cursor] == byte_class &&
                   inventory.proofs[cursor] == proof)
                ++cursor;
            const auto size = static_cast<std::uint64_t>(cursor - begin);
            const auto compile_class = precompile_class(byte_class, proof);
            const auto role = range_role(byte_class, source.permissions.writable);
            result.byte_counts[index(byte_class)] += size;
            result.precompile_counts[index(compile_class)] += size;
            result.role_counts[index(role)] += size;
            result.proof_counts[index(proof)] += size;
            group.byte_counts[index(byte_class)] += size;
            const auto address = source.virtual_address + static_cast<std::uint32_t>(begin);
            result.ranges.push_back({inventory.segment_index,
                                     address,
                                     source.file_offset + begin,
                                     size,
                                     byte_class,
                                     compile_class,
                                     role,
                                     proof,
                                     source.permissions.writable,
                                     count_between(static_references, address, size),
                                     0u,
                                     count_between(relocations, address, size),
                                     count_between(potential_targets, address, size),
                                     classification_reason(byte_class, proof)});
        }

        constexpr std::size_t page_size = 4096u;
        for (std::size_t begin = 0u; begin < inventory.bytes.size(); begin += page_size) {
            const auto size = std::min(page_size, inventory.bytes.size() - begin);
            std::array<std::size_t, mixed_range_role_count> roles{};
            std::array<std::array<std::size_t, range_proof_class_count>, mixed_range_role_count>
                role_proofs{};
            for (std::size_t offset = begin; offset < begin + size; ++offset) {
                const auto role = range_role(inventory.bytes[offset], source.permissions.writable);
                ++roles[index(role)];
                ++role_proofs[index(role)][index(inventory.proofs[offset])];
            }
            const auto dominant_index = static_cast<std::size_t>(
                std::distance(roles.begin(), std::max_element(roles.begin(), roles.end())));
            const auto proof_index = static_cast<std::size_t>(
                std::distance(role_proofs[dominant_index].begin(),
                              std::max_element(role_proofs[dominant_index].begin(),
                                               role_proofs[dominant_index].end())));
            const auto address = source.virtual_address + static_cast<std::uint32_t>(begin);
            result.pages.push_back({inventory.segment_index,
                                    address,
                                    size,
                                    source.permissions.writable,
                                    static_cast<MixedRangeRole>(dominant_index),
                                    static_cast<RangeProofClass>(proof_index),
                                    roles[dominant_index],
                                    roles[index(MixedRangeRole::Unknown)],
                                    count_between(static_references, address, size),
                                    0u,
                                    count_between(relocations, address, size),
                                    count_between(potential_targets, address, size),
                                    decode_density_ppm(source, begin, size),
                                    entropy_millibits(source, begin, size),
                                    distance_to_code(proven_code, address)});
        }
        result.groups.push_back(group);
    }
    return result;
}

const char* executable_byte_class_name(const ExecutableByteClass value) noexcept {
    constexpr std::array names{"proven_reachable_code",
                               "runtime_discovered_code",
                               "unreachable_decodable_code",
                               "embedded_data",
                               "literal_pool",
                               "jump_table",
                               "pointer_table",
                               "padding",
                               "overlay_candidate",
                               "module_candidate",
                               "compressed_or_encoded",
                               "unknown_executable"};
    return value < ExecutableByteClass::Count ? names[index(value)] : "unknown_executable";
}

const char* precompile_class_name(const PrecompileClass value) noexcept {
    constexpr std::array names{"initially_reachable",
                               "statically_discoverable",
                               "loadable_module",
                               "runtime_materializable",
                               "never_executed_data",
                               "unknown"};
    return value < PrecompileClass::Count ? names[index(value)] : "unknown";
}

const char* mixed_range_role_name(const MixedRangeRole value) noexcept {
    constexpr std::array names{"proven_code",
                               "reachable_code",
                               "runtime_materializable_code",
                               "literal_pool",
                               "jump_table",
                               "pointer_table",
                               "read_only_data",
                               "writable_data",
                               "padding",
                               "compressed_or_encoded",
                               "module_payload",
                               "unknown"};
    return value < MixedRangeRole::Count ? names[index(value)] : "unknown";
}

const char* range_proof_class_name(const RangeProofClass value) noexcept {
    constexpr std::array names{"proven", "guarded", "candidate", "unknown"};
    return value < RangeProofClass::Count ? names[index(value)] : "unknown";
}

std::string format_executable_inventory_json(const io::ExecutableImage& image,
                                             const ExecutableByteInventory& inventory,
                                             const bool include_local_details) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-executable-byte-inventory\",\"version\":2"
           << ",\"detail\":" << io::quote_json(include_local_details ? "local" : "redacted")
           << ",\"committed_executable_bytes\":" << inventory.committed_executable_bytes;
    const auto write_counts =
        [&](const char* name, const auto count, const auto& values, const auto name_function) {
            output << ",\"" << name << "\":{";
            for (std::size_t current = 0u; current < count; ++current) {
                if (current != 0u) output << ',';
                output << io::quote_json(name_function(current)) << ':' << values[current];
            }
            output << '}';
        };
    write_counts(
        "classes", executable_byte_class_count, inventory.byte_counts, [](const auto value) {
            return executable_byte_class_name(static_cast<ExecutableByteClass>(value));
        });
    write_counts("precompile_sets",
                 precompile_class_count,
                 inventory.precompile_counts,
                 [](const auto value) {
                     return precompile_class_name(static_cast<PrecompileClass>(value));
                 });
    write_counts("roles", mixed_range_role_count, inventory.role_counts, [](const auto value) {
        return mixed_range_role_name(static_cast<MixedRangeRole>(value));
    });
    write_counts(
        "proof_classes", range_proof_class_count, inventory.proof_counts, [](const auto value) {
            return range_proof_class_name(static_cast<RangeProofClass>(value));
        });

    output << ",\"groups\":[";
    for (std::size_t current = 0u; current < inventory.groups.size(); ++current) {
        if (current != 0u) output << ',';
        const auto& group = inventory.groups[current];
        output << "{\"segment_index\":" << group.segment_index << ",\"source_kind\":"
               << io::quote_json(io::image_source_kind_name(group.source_kind))
               << ",\"load_phase\":" << io::quote_json(io::image_load_phase_name(group.load_phase))
               << ",\"writable\":" << (group.writable ? "true" : "false")
               << ",\"committed_bytes\":" << group.committed_bytes
               << ",\"zero_fill_bytes\":" << group.zero_fill_bytes << '}';
    }
    output << ']';

    using PageKey = std::tuple<std::size_t,
                               io::ImageSourceKind,
                               io::ImageLoadPhase,
                               bool,
                               MixedRangeRole,
                               RangeProofClass,
                               bool,
                               bool,
                               bool,
                               std::uint8_t,
                               std::uint8_t,
                               std::uint8_t>;
    struct PageAggregate {
        std::uint64_t pages = 0u;
        std::uint64_t classified_bytes = 0u;
        std::uint64_t page_span_bytes = 0u;
        std::uint64_t decode_density_sum = 0u;
        std::uint64_t entropy_sum = 0u;
    };
    const auto entropy_bucket = [](const std::uint32_t value) -> std::uint8_t {
        if (value <= 1000u) return 0u;
        if (value <= 4000u) return 1u;
        if (value <= 7000u) return 2u;
        return 3u;
    };
    const auto decode_bucket = [](const std::uint32_t value) -> std::uint8_t {
        if (value <= 250'000u) return 0u;
        if (value <= 750'000u) return 1u;
        return 2u;
    };
    const auto proximity_bucket = [](const std::uint64_t value) -> std::uint8_t {
        if (value <= 4096u) return 0u;
        if (value <= 65'536u) return 1u;
        if (value != std::numeric_limits<std::uint64_t>::max()) return 2u;
        return 3u;
    };
    std::map<PageKey, PageAggregate> page_groups;
    for (const auto& page : inventory.pages) {
        const auto group = std::find_if(
            inventory.groups.begin(), inventory.groups.end(), [&](const auto& candidate) {
                return candidate.segment_index == page.segment_index;
            });
        const auto source_kind =
            group == inventory.groups.end() ? io::ImageSourceKind::Unknown : group->source_kind;
        const auto load_phase =
            group == inventory.groups.end() ? io::ImageLoadPhase::Initial : group->load_phase;
        auto& aggregate = page_groups[{page.segment_index,
                                       source_kind,
                                       load_phase,
                                       page.writable,
                                       page.dominant_role,
                                       page.strongest_proof,
                                       page.static_references != 0u,
                                       page.relocations != 0u,
                                       page.potential_control_flow_targets != 0u,
                                       entropy_bucket(page.entropy_millibits),
                                       decode_bucket(page.decode_density_ppm),
                                       proximity_bucket(page.distance_to_proven_code)}];
        ++aggregate.pages;
        aggregate.classified_bytes += page.dominant_role_bytes;
        aggregate.page_span_bytes += page.size;
        aggregate.decode_density_sum += page.decode_density_ppm;
        aggregate.entropy_sum += page.entropy_millibits;
    }
    output << ",\"page_groups\":[";
    std::size_t page_group_index = 0u;
    for (const auto& [key, aggregate] : page_groups) {
        if (page_group_index++ != 0u) output << ',';
        const auto [segment_index,
                    source_kind,
                    load_phase,
                    writable,
                    role,
                    proof,
                    has_static_reference,
                    has_relocation,
                    has_control_flow_target,
                    entropy,
                    decode,
                    proximity] = key;
        constexpr std::array entropy_names{"repeated", "low", "medium", "high"};
        constexpr std::array decode_names{"sparse", "mixed", "dense"};
        constexpr std::array proximity_names{"adjacent", "near", "far", "no_proven_code"};
        output << "{\"segment_index\":" << segment_index
               << ",\"source_kind\":" << io::quote_json(io::image_source_kind_name(source_kind))
               << ",\"load_phase\":" << io::quote_json(io::image_load_phase_name(load_phase))
               << ",\"writable\":" << (writable ? "true" : "false")
               << ",\"dominant_role\":" << io::quote_json(mixed_range_role_name(role))
               << ",\"proof\":" << io::quote_json(range_proof_class_name(proof))
               << ",\"has_static_reference\":" << (has_static_reference ? "true" : "false")
               << ",\"has_relocation\":" << (has_relocation ? "true" : "false")
               << ",\"has_control_flow_target\":" << (has_control_flow_target ? "true" : "false")
               << ",\"entropy_bucket\":" << io::quote_json(entropy_names[entropy])
               << ",\"decode_density_bucket\":" << io::quote_json(decode_names[decode])
               << ",\"proximity_bucket\":" << io::quote_json(proximity_names[proximity])
               << ",\"pages\":" << aggregate.pages
               << ",\"classified_bytes\":" << aggregate.classified_bytes
               << ",\"page_span_bytes\":" << aggregate.page_span_bytes
               << ",\"average_decode_density_ppm\":"
               << aggregate.decode_density_sum / aggregate.pages
               << ",\"average_entropy_millibits\":" << aggregate.entropy_sum / aggregate.pages
               << '}';
    }
    output << ']';

    if (include_local_details) {
        output << ",\"segments\":[";
        for (std::size_t current = 0u; current < image.segments().size(); ++current) {
            if (current != 0u) output << ',';
            const auto& segment = image.segments()[current];
            output << "{\"segment_index\":" << current
                   << ",\"name\":" << io::quote_json(segment.name)
                   << ",\"local_source_name\":" << io::quote_json(segment.local_source_name)
                   << ",\"address\":" << segment.virtual_address
                   << ",\"file_offset\":" << segment.file_offset
                   << ",\"file_size\":" << segment.bytes.size()
                   << ",\"memory_size\":" << segment.memory_size << '}';
        }
        output << "],\"ranges\":[";
        for (std::size_t current = 0u; current < inventory.ranges.size(); ++current) {
            if (current != 0u) output << ',';
            const auto& range = inventory.ranges[current];
            output << "{\"segment_index\":" << range.segment_index
                   << ",\"address\":" << range.address << ",\"file_offset\":" << range.file_offset
                   << ",\"size\":" << range.size
                   << ",\"class\":" << io::quote_json(executable_byte_class_name(range.byte_class))
                   << ",\"role\":" << io::quote_json(mixed_range_role_name(range.role))
                   << ",\"proof\":" << io::quote_json(range_proof_class_name(range.proof))
                   << ",\"precompile_set\":"
                   << io::quote_json(precompile_class_name(range.precompile_class))
                   << ",\"writable\":" << (range.writable ? "true" : "false")
                   << ",\"static_references\":" << range.static_references
                   << ",\"runtime_writes\":" << range.runtime_writes
                   << ",\"relocations\":" << range.relocations
                   << ",\"potential_control_flow_targets\":" << range.potential_control_flow_targets
                   << ",\"reason\":" << io::quote_json(range.reason) << '}';
        }
        output << "],\"pages\":[";
        for (std::size_t current = 0u; current < inventory.pages.size(); ++current) {
            if (current != 0u) output << ',';
            const auto& page = inventory.pages[current];
            output << "{\"segment_index\":" << page.segment_index << ",\"address\":" << page.address
                   << ",\"size\":" << page.size
                   << ",\"writable\":" << (page.writable ? "true" : "false")
                   << ",\"dominant_role\":"
                   << io::quote_json(mixed_range_role_name(page.dominant_role))
                   << ",\"proof\":" << io::quote_json(range_proof_class_name(page.strongest_proof))
                   << ",\"dominant_role_bytes\":" << page.dominant_role_bytes
                   << ",\"unknown_bytes\":" << page.unknown_bytes
                   << ",\"static_references\":" << page.static_references
                   << ",\"runtime_writes\":" << page.runtime_writes
                   << ",\"relocations\":" << page.relocations
                   << ",\"potential_control_flow_targets\":" << page.potential_control_flow_targets
                   << ",\"decode_density_ppm\":" << page.decode_density_ppm
                   << ",\"entropy_millibits\":" << page.entropy_millibits
                   << ",\"distance_to_proven_code\":" << page.distance_to_proven_code << '}';
        }
        output << ']';
    }
    output << "}\n";
    return output.str();
}

} // namespace katana::analysis
