#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/code_address.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace katana::analysis {
namespace {

constexpr std::size_t maximum_jump_table_entries = 4096u;

std::string snapshot_key(const katana::io::ExecutableImage& image,
                         const JumpTableEncoding encoding,
                         const std::uint32_t dispatch_address,
                         const std::uint32_t table_address,
                         const std::uint32_t target_base,
                         const std::size_t entry_count) {
    const auto entry_size = encoding == JumpTableEncoding::Absolute32 ? 4u : 2u;
    const auto byte_count =
        entry_count <= maximum_jump_table_entries ? entry_count * entry_size : 0u;
    const auto* segment =
        byte_count != 0u ? image.find_segment(table_address, byte_count) : nullptr;
    std::string digest = "invalid";
    if (segment != nullptr) {
        const auto offset = segment->byte_offset(table_address);
        if (offset && *offset <= segment->bytes.size() &&
            byte_count <= segment->bytes.size() - *offset) {
            digest = katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(segment->bytes.data() + *offset), byte_count));
        }
    }
    std::ostringstream key;
    key << static_cast<unsigned>(encoding) << ':' << dispatch_address << ':' << table_address << ':'
        << target_base << ':' << entry_count << ':' << digest;
    return key.str();
}

bool contiguous(const katana::sh4::DisassemblyLine& left,
                const katana::sh4::DisassemblyLine& right) {
    return left.address <= std::numeric_limits<std::uint32_t>::max() - 2u &&
           left.address + 2u == right.address;
}

bool outside_dispatch_path(const std::uint32_t target,
                           const std::uint32_t path_begin,
                           const std::uint32_t dispatch_address) {
    return target < path_begin || target > dispatch_address;
}

bool snapshot_candidate_source(const katana::io::ExecutableImage& image,
                               const katana::io::ImageSegment& segment) {
    if (!segment.permissions.readable) return false;
    if (segment.load_phase != katana::io::ImageLoadPhase::Initial ||
        segment.source_kind == katana::io::ImageSourceKind::RuntimeMemory)
        return false;
    if (!segment.permissions.writable) return true;
    return image.initial_snapshot_policy() ==
           katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent;
}

bool relative_register_transform(const katana::sh4::DecodedInstruction& instruction,
                                 const std::uint8_t register_index) {
    if (instruction.destination_register != register_index) return false;
    using K = katana::sh4::InstructionKind;
    switch (instruction.kind) {
    case K::AddImmediate:
    case K::AndRegister:
    case K::OrRegister:
    case K::XorRegister:
    case K::ExtendUnsignedWord:
    case K::ExtendSignedWord:
    case K::ShiftLogicalLeftOne:
    case K::ShiftLogicalRightOne:
    case K::ShiftArithmeticLeftOne:
    case K::ShiftArithmeticRightOne:
    case K::ShiftLogicalLeftTwo:
    case K::ShiftLogicalLeftEight:
    case K::ShiftLogicalLeftSixteen:
    case K::ShiftLogicalRightTwo:
    case K::ShiftLogicalRightEight:
    case K::ShiftLogicalRightSixteen:
        return true;
    default:
        return false;
    }
}

bool memory_derived_relative_register(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::size_t dispatch_index,
    const std::uint8_t register_index) {
    constexpr std::size_t instruction_budget = 48u;
    auto next_address = lines[dispatch_index].address;
    for (std::size_t distance = 1u;
         distance <= instruction_budget && distance <= dispatch_index;
         ++distance) {
        const auto& line = lines[dispatch_index - distance];
        if (line.address > std::numeric_limits<std::uint32_t>::max() - 2u ||
            line.address + 2u != next_address)
            return false;
        next_address = line.address;
        if ((general_register_write_mask(line.instruction) &
             static_cast<std::uint16_t>(1u << register_index)) == 0u)
            continue;
        if (relative_register_transform(line.instruction, register_index)) continue;
        if (line.instruction.destination_register != register_index) return false;
        using K = katana::sh4::InstructionKind;
        return line.instruction.kind == K::MovWordLoad ||
               line.instruction.kind == K::MovWordLoadPostIncrement ||
               line.instruction.kind == K::MovWordLoadDisplacement ||
               line.instruction.kind == K::MovWordLoadR0Indexed;
    }
    return false;
}

std::optional<katana::sh4::DecodedInstruction>
snapshot_instruction(const katana::io::ExecutableImage& image,
                     const std::uint32_t address,
                     const katana::io::ImageSegment* const expected_segment = nullptr) {
    const auto resolved = image.resolve_segment_address(address, 2u);
    if (!resolved.has_value()) return std::nullopt;
    const auto* segment = image.find_segment(*resolved, 2u);
    if (segment == nullptr || (expected_segment != nullptr && segment != expected_segment) ||
        !segment->permissions.executable || !snapshot_candidate_source(image, *segment))
        return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() || *offset > segment->bytes.size() - 2u) return std::nullopt;
    return katana::sh4::decode(katana::io::read_u16_le(segment->bytes, *offset));
}

bool fixed_stride_return_handler(const katana::io::ExecutableImage& image,
                                 const std::uint32_t address,
                                 const std::size_t stride,
                                 const katana::io::ImageSegment* const expected_segment) {
    if (stride < 4u || (stride & 1u) != 0u) return false;
    for (std::size_t offset = 0u; offset < stride; offset += 2u) {
        if (address > std::numeric_limits<std::uint32_t>::max() - offset) return false;
        const auto instruction = snapshot_instruction(
            image, address + static_cast<std::uint32_t>(offset), expected_segment);
        if (!instruction.has_value() || !instruction->is_known()) return false;
        if (offset == stride - 4u) {
            if (instruction->kind != katana::sh4::InstructionKind::Rts) return false;
        } else if (instruction->changes_control_flow()) {
            return false;
        }
    }
    return true;
}

bool fixed_stride_terminal_tail(const katana::io::ExecutableImage& image,
                                const std::uint32_t address,
                                const std::size_t stride,
                                const katana::io::ImageSegment* const expected_segment) {
    if (stride < 4u || (stride & 1u) != 0u) return false;
    for (std::size_t offset = 0u; offset < stride; offset += 2u) {
        if (address > std::numeric_limits<std::uint32_t>::max() - offset) return false;
        const auto instruction = snapshot_instruction(
            image, address + static_cast<std::uint32_t>(offset), expected_segment);
        if (!instruction.has_value() || !instruction->is_known()) return false;
        if (offset == 0u) {
            if (instruction->kind != katana::sh4::InstructionKind::Bra) return false;
        } else if (instruction->kind != katana::sh4::InstructionKind::Nop) {
            return false;
        }
    }
    const auto branch = snapshot_instruction(image, address, expected_segment);
    const auto target = branch.has_value()
                            ? katana::sh4::calculate_direct_branch_target(*branch, address)
                            : std::nullopt;
    return target.has_value() && validate_decode_candidate(image, *target).valid();
}

std::optional<std::size_t>
bounded_entry_count(const std::span<const katana::sh4::DisassemblyLine> lines,
                    const std::size_t scale_index,
                    const std::uint8_t index_register) {
    if (scale_index < 2u) return std::nullopt;
    for (std::size_t distance = 2u; distance <= 6u && distance <= scale_index; ++distance) {
        const auto compare_index = scale_index - distance;
        const auto& compare = lines[compare_index];
        if (compare.instruction.kind != katana::sh4::InstructionKind::CompareHigherOrSame ||
            compare.instruction.destination_register != index_register ||
            compare_index + 1u >= lines.size())
            continue;
        const auto& branch = lines[compare_index + 1u];
        if (!contiguous(compare, branch) || !branch.target_address.has_value()) continue;

        bool guarded = false;
        if (branch.instruction.kind == katana::sh4::InstructionKind::Bt &&
            compare_index + 2u == scale_index && contiguous(branch, lines[scale_index]) &&
            outside_dispatch_path(*branch.target_address,
                                  lines[scale_index].address,
                                  lines[scale_index + 4u].address)) {
            guarded = true;
        } else if (branch.instruction.kind == katana::sh4::InstructionKind::Bf &&
                   compare_index + 4u == scale_index &&
                   *branch.target_address == lines[scale_index].address) {
            const auto& fallback = lines[compare_index + 2u];
            const auto& delay = lines[compare_index + 3u];
            guarded = contiguous(branch, fallback) && contiguous(fallback, delay) &&
                      contiguous(delay, lines[scale_index]) &&
                      fallback.instruction.kind == katana::sh4::InstructionKind::Bra &&
                      fallback.target_address.has_value() &&
                      outside_dispatch_path(*fallback.target_address,
                                            lines[scale_index].address,
                                            lines[scale_index + 4u].address);
        }
        if (!guarded) continue;
        if (compare_index == 0u) return std::nullopt;
        const auto& bound = lines[compare_index - 1u];
        if (!contiguous(bound, compare) ||
            bound.instruction.kind != katana::sh4::InstructionKind::MovImmediate ||
            bound.instruction.destination_register != compare.instruction.source_register ||
            bound.instruction.immediate <= 0 ||
            static_cast<std::uint64_t>(bound.instruction.immediate) > maximum_jump_table_entries)
            return std::nullopt;
        return static_cast<std::size_t>(bound.instruction.immediate);
    }
    return std::nullopt;
}

} // namespace

JumpTableSnapshotCache::JumpTableSnapshotCache(const std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0u) throw std::invalid_argument("Jump-Table-Cache braucht Kapazitaet.");
    order_.reserve(capacity_);
    entries_.reserve(capacity_);
}

std::optional<JumpTableAnalysis> JumpTableSnapshotCache::load(const std::string_view key) {
    const auto found = entries_.find(std::string(key));
    if (found == entries_.end()) {
        ++counters_.misses;
        return std::nullopt;
    }
    ++counters_.hits;
    return found->second;
}

void JumpTableSnapshotCache::store(std::string key, JumpTableAnalysis analysis) {
    if (entries_.contains(key)) return;
    if (entries_.size() == capacity_) {
        entries_.erase(order_.front());
        order_.erase(order_.begin());
        ++counters_.evictions;
    }
    order_.push_back(key);
    entries_.emplace(std::move(key), std::move(analysis));
}

const JumpTableCacheCounters& JumpTableSnapshotCache::counters() const noexcept {
    return counters_;
}

std::size_t JumpTableSnapshotCache::size() const noexcept {
    return entries_.size();
}

JumpTableAnalysis analyze_jump_table(const katana::io::ExecutableImage& image,
                                     const std::uint32_t dispatch_address,
                                     const std::uint32_t table_address,
                                     const std::size_t entry_count,
                                     JumpTableSnapshotCache* const cache) {
    const auto key = snapshot_key(
        image, JumpTableEncoding::Absolute32, dispatch_address, table_address, 0u, entry_count);
    if (cache != nullptr) {
        if (auto hit = cache->load(key)) return *hit;
    }
    JumpTableAnalysis analysis;
    analysis.dispatch_address = dispatch_address;
    analysis.table_address = table_address;
    analysis.requested_entries = entry_count;

    if (entry_count == 0u || entry_count > maximum_jump_table_entries) {
        analysis.reason = "entry-count-out-of-range";
        return analysis;
    }
    const auto table_end =
        static_cast<std::uint64_t>(table_address) + static_cast<std::uint64_t>(entry_count) * 4u;
    if ((table_address & 3u) != 0u || table_end > 0x100000000ull) {
        analysis.reason = "table-range-invalid";
        return analysis;
    }

    const auto byte_count = static_cast<std::size_t>(entry_count * 4u);
    const auto* segment = image.find_segment(table_address, byte_count);
    const auto offset = segment != nullptr ? segment->byte_offset(table_address) : std::nullopt;
    if (segment == nullptr || !segment->permissions.readable || segment->permissions.writable ||
        !offset.has_value() || *offset > segment->bytes.size() ||
        byte_count > segment->bytes.size() - *offset) {
        analysis.reason = segment != nullptr && segment->permissions.writable
                              ? "table-segment-writable"
                              : "table-range-not-immutable";
        return analysis;
    }

    analysis.entries.reserve(entry_count);
    for (std::size_t index = 0u; index < entry_count; ++index) {
        const auto entry_address = table_address + static_cast<std::uint32_t>(index * 4u);
        JumpTableEntry entry;
        entry.index = index;
        entry.entry_address = entry_address;
        entry.target = static_cast<std::uint32_t>(
                           katana::io::read_u16_le(segment->bytes, *offset + index * 4u)) |
                       (static_cast<std::uint32_t>(
                            katana::io::read_u16_le(segment->bytes, *offset + index * 4u + 2u))
                        << 16u);
        const auto validation = validate_decode_candidate(image, entry.target);
        if (!validation.valid()) {
            entry.reason = code_address_status_name(validation.status);
            analysis.entries.push_back(std::move(entry));
            analysis.reason = "table-entry-rejected";
            continue;
        }
        entry.target = validation.resolved_address;
        entry.accepted = true;
        entry.reason = "bounded-absolute-target";
        analysis.entries.push_back(std::move(entry));
    }

    analysis.resolved = analysis.entries.size() == entry_count;
    for (const auto& entry : analysis.entries) {
        analysis.resolved = analysis.resolved && entry.accepted;
    }
    if (analysis.resolved) {
        analysis.reason = "bounded-table";
    } else if (analysis.reason.empty()) {
        analysis.reason = "table-entry-rejected";
    }
    if (cache != nullptr) cache->store(key, analysis);
    return analysis;
}

namespace {

JumpTableAnalysis analyze_relative_jump_table_impl(const katana::io::ExecutableImage& image,
                                                   const std::uint32_t dispatch_address,
                                                   const std::uint32_t table_address,
                                                   const std::uint32_t target_base,
                                                   const std::size_t entry_count) {
    JumpTableAnalysis analysis;
    analysis.dispatch_address = dispatch_address;
    analysis.table_address = table_address;
    analysis.target_base = target_base;
    analysis.requested_entries = entry_count;
    analysis.encoding = JumpTableEncoding::SignedRelative16;

    if (entry_count == 0u || entry_count > maximum_jump_table_entries) {
        analysis.reason = "entry-count-out-of-range";
        return analysis;
    }
    const auto byte_count = static_cast<std::uint64_t>(entry_count) * 2u;
    const auto table_end = static_cast<std::uint64_t>(table_address) + byte_count;
    if ((table_address & 1u) != 0u || table_end > 0x100000000ull) {
        analysis.reason = "table-range-invalid";
        return analysis;
    }
    const auto* segment = image.find_segment(table_address, static_cast<std::size_t>(byte_count));
    const auto offset = segment != nullptr ? segment->byte_offset(table_address) : std::nullopt;
    if (segment == nullptr || !segment->permissions.readable || segment->permissions.writable ||
        !offset.has_value() || *offset > segment->bytes.size() ||
        byte_count > segment->bytes.size() - *offset) {
        analysis.reason = segment != nullptr && segment->permissions.writable
                              ? "table-segment-writable"
                              : "table-range-not-immutable";
        return analysis;
    }

    analysis.entries.reserve(entry_count);
    for (std::size_t index = 0u; index < entry_count; ++index) {
        JumpTableEntry entry;
        entry.index = index;
        entry.entry_address = table_address + static_cast<std::uint32_t>(index * 2u);
        const auto raw = katana::io::read_u16_le(segment->bytes, *offset + index * 2u);
        const auto relative = static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
        const auto target = static_cast<std::int64_t>(target_base) + relative;
        if (target < 0 || target > std::numeric_limits<std::uint32_t>::max()) {
            entry.reason = "target-address-overflow";
            analysis.reason = "table-entry-rejected";
            analysis.entries.push_back(std::move(entry));
            continue;
        }
        entry.target = static_cast<std::uint32_t>(target);
        const auto validation = validate_decode_candidate(image, entry.target);
        if (!validation.valid()) {
            entry.reason = code_address_status_name(validation.status);
            analysis.reason = "table-entry-rejected";
        } else {
            entry.target = validation.resolved_address;
            entry.accepted = true;
            entry.reason = "bounded-signed-relative-target";
        }
        analysis.entries.push_back(std::move(entry));
    }
    analysis.resolved = analysis.entries.size() == entry_count;
    for (const auto& entry : analysis.entries)
        analysis.resolved = analysis.resolved && entry.accepted;
    if (analysis.resolved) {
        analysis.reason = "bounded-signed-relative-table";
    } else if (analysis.reason.empty()) {
        analysis.reason = "table-entry-rejected";
    }
    return analysis;
}

} // namespace

JumpTableAnalysis analyze_relative_jump_table(const katana::io::ExecutableImage& image,
                                              const std::uint32_t dispatch_address,
                                              const std::uint32_t table_address,
                                              const std::uint32_t target_base,
                                              const std::size_t entry_count,
                                              JumpTableSnapshotCache* const cache) {
    const auto key = snapshot_key(image,
                                  JumpTableEncoding::SignedRelative16,
                                  dispatch_address,
                                  table_address,
                                  target_base,
                                  entry_count);
    if (cache != nullptr) {
        if (auto hit = cache->load(key)) return *hit;
    }
    auto analysis = analyze_relative_jump_table_impl(
        image, dispatch_address, table_address, target_base, entry_count);
    if (cache != nullptr) cache->store(key, analysis);
    return analysis;
}

std::optional<JumpTableAnalysis>
recognize_bounded_relative_jump_table(const katana::io::ExecutableImage& image,
                                      const std::span<const katana::sh4::DisassemblyLine> lines,
                                      const std::size_t dispatch_index,
                                      JumpTableSnapshotCache* const cache) {
    if (dispatch_index < 4u || dispatch_index >= lines.size()) return std::nullopt;
    const auto& dispatch = lines[dispatch_index];
    const auto& load = lines[dispatch_index - 1u];
    const auto& table_base = lines[dispatch_index - 2u];
    const auto& copy_index = lines[dispatch_index - 3u];
    const auto& scale_index = lines[dispatch_index - 4u];
    if (!contiguous(scale_index, copy_index) || !contiguous(copy_index, table_base) ||
        !contiguous(table_base, load) || !contiguous(load, dispatch) ||
        dispatch.instruction.kind != katana::sh4::InstructionKind::Braf ||
        load.instruction.kind != katana::sh4::InstructionKind::MovWordLoadR0Indexed ||
        load.instruction.destination_register != dispatch.instruction.branch_register ||
        table_base.instruction.kind != katana::sh4::InstructionKind::MoveAddressPcRelative ||
        copy_index.instruction.kind != katana::sh4::InstructionKind::MovRegister ||
        copy_index.instruction.destination_register != load.instruction.source_register ||
        copy_index.instruction.destination_register == 0u ||
        scale_index.instruction.kind != katana::sh4::InstructionKind::ShiftLogicalLeftOne ||
        scale_index.instruction.destination_register != copy_index.instruction.source_register)
        return std::nullopt;

    const auto entry_count = bounded_entry_count(
        lines, dispatch_index - 4u, scale_index.instruction.destination_register);
    if (!entry_count.has_value()) return std::nullopt;
    const auto table_address = ((table_base.address + 4u) & ~3u) +
                               static_cast<std::uint32_t>(table_base.instruction.displacement);
    return analyze_relative_jump_table(
        image, dispatch.address, table_address, dispatch.address + 4u, *entry_count, cache);
}

std::optional<JumpTableAnalysis>
recognize_snapshot_absolute_jump_table_candidates(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::size_t dispatch_index) {
    if (dispatch_index < 2u || dispatch_index >= lines.size()) return std::nullopt;
    const auto& dispatch = lines[dispatch_index];
    if (dispatch.instruction.kind != katana::sh4::InstructionKind::Jmp &&
        dispatch.instruction.kind != katana::sh4::InstructionKind::Jsr)
        return std::nullopt;

    std::optional<std::size_t> indexed_load_index;
    for (std::size_t distance = 1u; distance <= 3u && distance <= dispatch_index; ++distance) {
        const auto candidate_index = dispatch_index - distance;
        const auto& candidate = lines[candidate_index];
        if (!contiguous(candidate, lines[candidate_index + 1u])) break;
        if (candidate.instruction.kind ==
                katana::sh4::InstructionKind::MovLongLoadR0Indexed &&
            candidate.instruction.destination_register == dispatch.instruction.branch_register) {
            bool clobbered = false;
            for (auto index = candidate_index + 1u; index < dispatch_index; ++index) {
                const auto& between = lines[index].instruction;
                if (between.changes_control_flow() ||
                    (general_register_write_mask(between) &
                     static_cast<std::uint16_t>(
                         1u << dispatch.instruction.branch_register)) != 0u ||
                    (between.kind != katana::sh4::InstructionKind::Nop &&
                     !(between.kind == katana::sh4::InstructionKind::AddImmediate &&
                       between.destination_register == 0u))) {
                    clobbered = true;
                    break;
                }
            }
            if (!clobbered) indexed_load_index = candidate_index;
            break;
        }
    }
    if (!indexed_load_index.has_value() || *indexed_load_index == 0u) return std::nullopt;

    const auto& indexed_load = lines[*indexed_load_index];
    const auto& base_load = lines[*indexed_load_index - 1u];
    if (!contiguous(base_load, indexed_load) ||
        base_load.instruction.kind !=
            katana::sh4::InstructionKind::MovLongLoadPcRelative ||
        base_load.instruction.destination_register != indexed_load.instruction.source_register ||
        indexed_load.instruction.source_register == 0u)
        return std::nullopt;

    const auto literal_address =
        ((base_load.address + 4u) & ~3u) +
        static_cast<std::uint32_t>(base_load.instruction.displacement);
    const auto resolved_literal = image.resolve_segment_address(literal_address, 4u);
    if (!resolved_literal.has_value()) return std::nullopt;
    const auto* literal_segment = image.find_segment(*resolved_literal, 4u);
    if (literal_segment == nullptr || !snapshot_candidate_source(image, *literal_segment))
        return std::nullopt;

    const auto table_pointer = image.read_u32_le(*resolved_literal);
    const auto resolved_table = image.resolve_segment_address(table_pointer, 4u);
    if (!resolved_table.has_value() || (*resolved_table & 3u) != 0u) return std::nullopt;
    const auto* table_segment = image.find_segment(*resolved_table, 4u);
    if (table_segment == nullptr || !snapshot_candidate_source(image, *table_segment))
        return std::nullopt;
    const auto table_offset = table_segment->byte_offset(*resolved_table);
    if (!table_offset.has_value() || *table_offset > table_segment->bytes.size())
        return std::nullopt;

    const auto available_entries = (table_segment->bytes.size() - *table_offset) / 4u;
    const auto scan_limit = std::min(available_entries, maximum_jump_table_entries);
    JumpTableAnalysis analysis;
    analysis.dispatch_address = dispatch.address;
    analysis.table_address = *resolved_table;
    analysis.dispatch_kind =
        dispatch.instruction.kind == katana::sh4::InstructionKind::Jsr
            ? JumpTableDispatchKind::Call
            : JumpTableDispatchKind::Jump;
    analysis.encoding = JumpTableEncoding::Absolute32;
    analysis.aot_candidates_only = true;
    analysis.evidence = ControlFlowEvidence::GuardedPartial;
    analysis.entries.reserve(scan_limit);
    for (std::size_t index = 0u; index < scan_limit; ++index) {
        const auto offset = *table_offset + index * 4u;
        const auto target = static_cast<std::uint32_t>(
                                katana::io::read_u16_le(table_segment->bytes, offset)) |
                            (static_cast<std::uint32_t>(katana::io::read_u16_le(
                                 table_segment->bytes, offset + 2u))
                             << 16u);
        const auto validation = validate_decode_candidate(image, target);
        if (!validation.valid()) break;
        analysis.entries.push_back({index,
                                    *resolved_table + static_cast<std::uint32_t>(index * 4u),
                                    validation.resolved_address,
                                    true,
                                    "snapshot-absolute-target"});
    }
    if (analysis.entries.size() < 2u ||
        (analysis.entries.size() == maximum_jump_table_entries &&
         available_entries > maximum_jump_table_entries))
        return std::nullopt;
    analysis.requested_entries = analysis.entries.size();
    analysis.resolved = true;
    analysis.reason = "snapshot-absolute-pointer-candidates";
    return analysis;
}

std::optional<RelativeCallIslandCandidates>
recognize_snapshot_relative_call_island_candidates(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::size_t dispatch_index) {
    constexpr std::size_t minimum_handlers = 4u;
    constexpr std::size_t maximum_handlers = 32u;
    constexpr std::uint32_t minimum_start_distance = 4u;
    constexpr std::uint32_t maximum_start_distance = 128u;
    constexpr std::size_t minimum_stride = 4u;
    constexpr std::size_t maximum_stride = 16u;

    if (dispatch_index >= lines.size()) return std::nullopt;
    const auto& dispatch = lines[dispatch_index];
    if (dispatch.instruction.kind != katana::sh4::InstructionKind::Bsrf ||
        dispatch.instruction.branch_register >= 16u ||
        !memory_derived_relative_register(
            lines, dispatch_index, dispatch.instruction.branch_register))
        return std::nullopt;

    const auto resolved_dispatch = image.resolve_segment_address(dispatch.address, 2u);
    if (!resolved_dispatch.has_value()) return std::nullopt;
    const auto* dispatch_segment = image.find_segment(*resolved_dispatch, 2u);
    if (dispatch_segment == nullptr || !dispatch_segment->permissions.executable ||
        !snapshot_candidate_source(image, *dispatch_segment))
        return std::nullopt;

    if (dispatch.address > std::numeric_limits<std::uint32_t>::max() - 4u)
        return std::nullopt;
    const auto relative_base = dispatch.address + 4u;
    std::vector<std::pair<std::size_t, RelativeCallIslandCandidates>> matches;
    std::size_t best_return_handlers = 0u;
    for (std::uint32_t distance = minimum_start_distance;
         distance <= maximum_start_distance;
         distance += 2u) {
        if (dispatch.address > std::numeric_limits<std::uint32_t>::max() - distance) break;
        const auto first_target = dispatch.address + distance;
        for (std::size_t stride = minimum_stride; stride <= maximum_stride; stride += 2u) {
            if ((first_target - relative_base) % stride != 0u) continue;
            const auto resolved_first = image.resolve_segment_address(first_target, 2u);
            if (!resolved_first.has_value()) continue;
            const auto* segment = image.find_segment(*resolved_first, 2u);
            if (segment == nullptr || segment != dispatch_segment ||
                !segment->permissions.executable ||
                !snapshot_candidate_source(image, *segment))
                continue;
            if (first_target >= stride &&
                fixed_stride_return_handler(image,
                                            first_target - static_cast<std::uint32_t>(stride),
                                            stride,
                                            segment))
                continue;
            std::size_t return_handlers = 0u;
            for (; return_handlers < maximum_handlers; ++return_handlers) {
                const auto delta = static_cast<std::uint64_t>(return_handlers) * stride;
                if (delta > std::numeric_limits<std::uint32_t>::max() - first_target) break;
                if (!fixed_stride_return_handler(
                        image,
                        first_target + static_cast<std::uint32_t>(delta),
                        stride,
                        segment))
                    break;
            }
            if (return_handlers < minimum_handlers) continue;
            const auto tail_delta = static_cast<std::uint64_t>(return_handlers) * stride;
            if (tail_delta > std::numeric_limits<std::uint32_t>::max() - first_target) continue;
            const auto tail = first_target + static_cast<std::uint32_t>(tail_delta);
            if (return_handlers == maximum_handlers &&
                fixed_stride_return_handler(image, tail, stride, segment))
                continue;
            if (!fixed_stride_terminal_tail(image, tail, stride, segment)) continue;

            RelativeCallIslandCandidates candidate;
            candidate.dispatch_address = dispatch.address;
            candidate.first_target = first_target;
            candidate.stride = stride;
            candidate.targets.reserve(return_handlers + 1u);
            for (std::size_t index = 0u; index < return_handlers; ++index) {
                candidate.targets.push_back(
                    first_target + static_cast<std::uint32_t>(index * stride));
            }
            candidate.targets.push_back(tail);
            candidate.terminal_tail_transfer = true;
            candidate.reason = "snapshot-relative-call-island-candidates";
            if (return_handlers > best_return_handlers) {
                matches.clear();
                best_return_handlers = return_handlers;
            }
            if (return_handlers == best_return_handlers)
                matches.emplace_back(return_handlers, std::move(candidate));
        }
    }
    if (matches.size() != 1u) return std::nullopt;
    return std::move(matches.front().second);
}

const char* jump_table_encoding_name(const JumpTableEncoding encoding) noexcept {
    switch (encoding) {
    case JumpTableEncoding::Absolute32:
        return "absolute32";
    case JumpTableEncoding::SignedRelative16:
        return "signed-relative16";
    }
    return "unknown";
}

} // namespace katana::analysis
