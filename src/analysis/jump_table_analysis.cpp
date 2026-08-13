#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/code_address.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction.hpp"
#include "snapshot_pointer_candidates.hpp"

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
    const auto entry_size =
        encoding == JumpTableEncoding::SignedRelative16 ? 2u : 4u;
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
    if (segment.source_kind == katana::io::ImageSourceKind::RuntimeMemory)
        return false;
    // Latent disc modules are analyzed from exact transformed bytes and are
    // rebound by both encoded and decoded SHA-256 at runtime. Their mixed
    // code/data segment is writable after loading, so discovered table values
    // remain guarded candidates rather than fixed CFG truth. Admitting this
    // source class lets the existing bounded BRAF recognizer recover all case
    // blocks while any later table mutation still fails the generated target
    // guard instead of inventing executable code.
    if (segment.source_kind == katana::io::ImageSourceKind::DiscModule &&
        segment.load_phase == katana::io::ImageLoadPhase::RuntimeModule)
        return true;
    if (segment.load_phase == katana::io::ImageLoadPhase::Initial &&
        !segment.permissions.writable)
        return true;
    return segment.load_phase == katana::io::ImageLoadPhase::Initial &&
           image.initial_snapshot_policy() ==
               katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent;
}

std::optional<std::uint32_t> snapshot_pc_relative_long_value(
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& load) {
    if (load.instruction.kind !=
        katana::sh4::InstructionKind::MovLongLoadPcRelative)
        return std::nullopt;
    const auto literal_address64 =
        ((static_cast<std::uint64_t>(load.address) + 4u) & ~std::uint64_t{3u}) +
        static_cast<std::uint32_t>(load.instruction.displacement);
    if (literal_address64 > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto resolved_literal = image.resolve_segment_address(
        static_cast<std::uint32_t>(literal_address64), 4u);
    const auto* literal_segment = resolved_literal.has_value()
                                      ? image.find_segment(*resolved_literal, 4u)
                                      : nullptr;
    if (literal_segment == nullptr ||
        !snapshot_candidate_source(image, *literal_segment))
        return std::nullopt;
    const auto value = image.read_u32_le(*resolved_literal);
    return image.resolve_segment_address(value, 2u).has_value()
               ? std::optional<std::uint32_t>{value}
               : std::nullopt;
}

bool resolved_outside_dispatch_path(const katana::io::ExecutableImage& image,
                                    const std::uint32_t target,
                                    const std::uint32_t path_begin,
                                    const std::uint32_t dispatch_address) {
    const auto resolved_target = image.resolve_segment_address(target, 2u);
    const auto resolved_begin = image.resolve_segment_address(path_begin, 2u);
    const auto resolved_dispatch = image.resolve_segment_address(dispatch_address, 2u);
    return resolved_target.has_value() && resolved_begin.has_value() &&
           resolved_dispatch.has_value() &&
           outside_dispatch_path(*resolved_target, *resolved_begin, *resolved_dispatch);
}

bool bounded_fallback_exits_dispatch_path(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::size_t fallback_begin,
    const std::size_t scale_index,
    const std::size_t dispatch_index) {
    if (fallback_begin >= scale_index || scale_index < 2u ||
        dispatch_index <= scale_index || dispatch_index >= lines.size())
        return false;
    for (auto index = fallback_begin; index < scale_index; ++index) {
        if (index + 1u >= lines.size() || !contiguous(lines[index], lines[index + 1u]))
            return false;
    }

    const auto terminal_index = scale_index - 2u;
    const auto delay_index = scale_index - 1u;
    if (terminal_index < fallback_begin || !lines[delay_index].is_delay_slot)
        return false;
    for (auto index = fallback_begin; index < terminal_index; ++index) {
        if (lines[index].instruction.changes_control_flow()) return false;
    }

    const auto& terminal = lines[terminal_index];
    std::optional<std::uint32_t> target;
    if (terminal.instruction.kind == katana::sh4::InstructionKind::Bra) {
        target = terminal.target_address;
    } else if (terminal.instruction.kind == katana::sh4::InstructionKind::Jmp &&
               terminal_index != fallback_begin) {
        const auto& target_load = lines[terminal_index - 1u];
        if (contiguous(target_load, terminal) &&
            target_load.instruction.destination_register ==
                terminal.instruction.branch_register)
            target = snapshot_pc_relative_long_value(image, target_load);
    }
    return target.has_value() &&
           resolved_outside_dispatch_path(image,
                                          *target,
                                          lines[scale_index].address,
                                          lines[dispatch_index].address);
}

std::optional<JumpTableAnalysis>
recognize_snapshot_displaced_absolute_pointer_candidates(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::size_t dispatch_index) {
    constexpr std::uint32_t provenance_byte_budget = 14u;
    constexpr detail::SnapshotPointerCandidateScanPolicy scan_policy{
        .minimum_entries = 1u,
        .maximum_scanned_slots = 8u,
        .maximum_skipped_slots = 2u,
        .maximum_consecutive_skipped_slots = 1u,
        .treat_null_as_reserved = true,
        .reject_truncated_scan = false,
    };
    if (dispatch_index < 2u || dispatch_index >= lines.size()) return std::nullopt;
    const auto& dispatch = lines[dispatch_index];
    const auto& table_load = lines[dispatch_index - 1u];
    if (!contiguous(table_load, dispatch) ||
        table_load.instruction.kind !=
            katana::sh4::InstructionKind::MovLongLoadDisplacement ||
        table_load.instruction.destination_register !=
            dispatch.instruction.branch_register)
        return std::nullopt;

    auto tracked_register = table_load.instruction.source_register;
    const katana::sh4::DisassemblyLine* base_load = nullptr;
    bool copied_base_register = false;
    auto next_address = table_load.address;
    for (auto cursor = dispatch_index - 1u; cursor > 0u;) {
        --cursor;
        const auto& candidate = lines[cursor];
        if (candidate.address > std::numeric_limits<std::uint32_t>::max() - 2u ||
            candidate.address + 2u != next_address)
            break;
        next_address = candidate.address;
        if (candidate.address > dispatch.address ||
            dispatch.address - candidate.address > provenance_byte_budget)
            break;
        if (candidate.instruction.changes_control_flow()) break;
        if ((general_register_write_mask(candidate.instruction) &
             static_cast<std::uint16_t>(1u << tracked_register)) == 0u)
            continue;
        if (candidate.instruction.kind ==
                katana::sh4::InstructionKind::MovRegister &&
            candidate.instruction.destination_register == tracked_register) {
            copied_base_register = true;
            tracked_register = candidate.instruction.source_register;
            continue;
        }
        if (candidate.instruction.kind ==
                katana::sh4::InstructionKind::MovLongLoadPcRelative &&
            candidate.instruction.destination_register == tracked_register) {
            base_load = &candidate;
        }
        break;
    }
    if (base_load == nullptr || !copied_base_register || base_load->is_delay_slot)
        return std::nullopt;

    const auto resolved_base_load = image.resolve_segment_address(base_load->address, 2u);
    const auto* base_load_segment =
        resolved_base_load.has_value() ? image.find_segment(*resolved_base_load, 2u) : nullptr;
    if (base_load_segment == nullptr || !base_load_segment->permissions.executable ||
        !snapshot_candidate_source(image, *base_load_segment))
        return std::nullopt;
    const auto literal_address64 =
        ((static_cast<std::uint64_t>(base_load->address) + 4u) & ~std::uint64_t{3u}) +
        static_cast<std::uint32_t>(base_load->instruction.displacement);
    if (literal_address64 > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto resolved_literal = image.resolve_segment_address(
        static_cast<std::uint32_t>(literal_address64), 4u);
    const auto* literal_segment =
        resolved_literal.has_value() ? image.find_segment(*resolved_literal, 4u) : nullptr;
    if (literal_segment == nullptr ||
        !snapshot_candidate_source(image, *literal_segment))
        return std::nullopt;

    const auto table_address64 =
        static_cast<std::uint64_t>(image.read_u32_le(*resolved_literal)) +
        static_cast<std::uint32_t>(table_load.instruction.displacement);
    if (table_address64 > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    auto result = detail::analyze_snapshot_pointer_candidates(
        image,
        dispatch.address,
        static_cast<std::uint32_t>(table_address64),
        dispatch.instruction.kind == katana::sh4::InstructionKind::Jsr
            ? JumpTableDispatchKind::Call
            : JumpTableDispatchKind::Jump,
        scan_policy);
    if (result.has_value())
        result->reason = "snapshot-displaced-absolute-pointer-candidates";
    return result;
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
                    const katana::io::ExecutableImage& image,
                    const std::size_t scale_index,
                    const std::size_t dispatch_index,
                    const std::uint8_t index_register) {
    if (scale_index < 2u || dispatch_index <= scale_index ||
        dispatch_index >= lines.size())
        return std::nullopt;
    for (std::size_t distance = 2u; distance <= 16u && distance <= scale_index; ++distance) {
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
                                  lines[dispatch_index].address)) {
            guarded = true;
        } else if (branch.instruction.kind == katana::sh4::InstructionKind::Bf &&
                   *branch.target_address == lines[scale_index].address &&
                   compare_index + 2u < scale_index) {
            guarded = contiguous(branch, lines[compare_index + 2u]) &&
                      bounded_fallback_exits_dispatch_path(image,
                                                           lines,
                                                           compare_index + 2u,
                                                           scale_index,
                                                           dispatch_index);
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

void JumpTableSnapshotCache::bind_image(const katana::io::ExecutableImage& image) noexcept {
    if (image_ == &image) return;
    image_ = &image;
    order_.clear();
    entries_.clear();
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
    if (cache != nullptr) cache->bind_image(image);
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
                                                   const std::size_t entry_count,
                                                   const bool initial_snapshot_candidates = false) {
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
    const bool writable_snapshot_source =
        initial_snapshot_candidates && segment != nullptr && segment->permissions.writable &&
        snapshot_candidate_source(image, *segment);
    if (segment == nullptr || !segment->permissions.readable ||
        (segment->permissions.writable && !writable_snapshot_source) ||
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
        if (!validation.valid() ||
            (initial_snapshot_candidates &&
             (validation.segment == nullptr ||
              !snapshot_candidate_source(image, *validation.segment)))) {
            entry.reason = code_address_status_name(validation.status);
            if (validation.valid()) entry.reason = "target-not-in-initial-snapshot";
            analysis.reason = "table-entry-rejected";
        } else {
            entry.target = validation.resolved_address;
            entry.accepted = true;
            entry.reason = initial_snapshot_candidates
                               ? "snapshot-signed-relative16-target"
                               : "bounded-signed-relative-target";
        }
        analysis.entries.push_back(std::move(entry));
    }
    analysis.resolved = analysis.entries.size() == entry_count;
    for (const auto& entry : analysis.entries)
        analysis.resolved = analysis.resolved && entry.accepted;
    if (analysis.resolved) {
        if (initial_snapshot_candidates) {
            analysis.aot_candidates_only = true;
            analysis.evidence = ControlFlowEvidence::GuardedPartial;
            analysis.reason = "snapshot-signed-relative16-candidates";
        } else {
            analysis.reason = "bounded-signed-relative-table";
        }
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
    if (cache != nullptr) cache->bind_image(image);
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

JumpTableAnalysis analyze_declared_jump_table(
    const katana::io::ExecutableImage& image,
    const std::uint32_t dispatch_address,
    const std::uint32_t table_address,
    const std::uint32_t target_base,
    const std::size_t entry_count,
    const std::uint32_t entry_stride,
    const JumpTableEncoding encoding) {
    JumpTableAnalysis analysis;
    analysis.dispatch_address = dispatch_address;
    analysis.table_address = table_address;
    analysis.target_base = target_base;
    analysis.requested_entries = entry_count;
    analysis.encoding = encoding;

    const auto width =
        encoding == JumpTableEncoding::SignedRelative16 ? 2u : 4u;
    if (entry_count == 0u || entry_count > maximum_jump_table_entries) {
        analysis.reason = "entry-count-out-of-range";
        return analysis;
    }
    if (entry_stride < width || (entry_stride % width) != 0u ||
        (table_address & (width - 1u)) != 0u ||
        (encoding == JumpTableEncoding::Absolute32 && target_base != 0u)) {
        analysis.reason = "declared-table-layout-invalid";
        return analysis;
    }
    const auto byte_count =
        static_cast<std::uint64_t>(entry_count - 1u) * entry_stride + width;
    const auto table_end =
        static_cast<std::uint64_t>(table_address) + byte_count;
    if (table_end > 0x1'0000'0000ull ||
        byte_count > std::numeric_limits<std::size_t>::max()) {
        analysis.reason = "table-range-invalid";
        return analysis;
    }
    const auto* segment =
        image.find_segment(table_address, static_cast<std::size_t>(byte_count));
    const auto offset =
        segment != nullptr ? segment->byte_offset(table_address) : std::nullopt;
    const bool writable_snapshot_source =
        segment != nullptr && segment->permissions.writable &&
        snapshot_candidate_source(image, *segment);
    if (segment == nullptr || !segment->permissions.readable ||
        (segment->permissions.writable && !writable_snapshot_source) ||
        !offset.has_value() ||
        *offset > segment->bytes.size() ||
        byte_count > segment->bytes.size() - *offset) {
        analysis.reason =
            segment != nullptr && segment->permissions.writable
                ? "table-segment-writable"
                : "table-range-not-immutable";
        return analysis;
    }

    analysis.entries.reserve(entry_count);
    for (std::size_t index = 0u; index < entry_count; ++index) {
        const auto byte_offset =
            *offset + static_cast<std::size_t>(index) * entry_stride;
        const auto entry_address64 =
            static_cast<std::uint64_t>(table_address) +
            static_cast<std::uint64_t>(index) * entry_stride;
        JumpTableEntry entry;
        entry.index = index;
        entry.entry_address = static_cast<std::uint32_t>(entry_address64);
        const auto low =
            katana::io::read_u16_le(segment->bytes, byte_offset);
        std::int64_t target = 0;
        if (encoding == JumpTableEncoding::SignedRelative16) {
            target = static_cast<std::int64_t>(target_base) +
                     static_cast<std::int16_t>(low);
        } else {
            const auto high =
                katana::io::read_u16_le(segment->bytes, byte_offset + 2u);
            const auto raw = static_cast<std::uint32_t>(low) |
                             (static_cast<std::uint32_t>(high) << 16u);
            target = encoding == JumpTableEncoding::SignedRelative32
                         ? static_cast<std::int64_t>(target_base) +
                               static_cast<std::int32_t>(raw)
                         : static_cast<std::int64_t>(raw);
        }
        if (target < 0 ||
            target > std::numeric_limits<std::uint32_t>::max()) {
            entry.reason = "target-address-overflow";
            analysis.reason = "table-entry-rejected";
            analysis.entries.push_back(std::move(entry));
            continue;
        }
        entry.target = static_cast<std::uint32_t>(target);
        const auto validation = validate_decode_candidate(image, entry.target);
        if (!validation.valid() ||
            (writable_snapshot_source &&
             (validation.segment == nullptr ||
              !snapshot_candidate_source(image, *validation.segment)))) {
            entry.reason = code_address_status_name(validation.status);
            if (validation.valid())
                entry.reason = "target-not-in-initial-snapshot";
            analysis.reason = "table-entry-rejected";
        } else {
            entry.target = validation.resolved_address;
            entry.accepted = true;
            entry.reason = writable_snapshot_source
                ? "identity-bound-declared-snapshot-target"
                : "identity-bound-declared-target";
        }
        analysis.entries.push_back(std::move(entry));
    }
    analysis.resolved = analysis.entries.size() == entry_count &&
                        std::all_of(
                            analysis.entries.begin(),
                            analysis.entries.end(),
                            [](const auto& entry) {
                                return entry.accepted;
                            });
    if (analysis.resolved) {
        if (writable_snapshot_source) {
            analysis.aot_candidates_only = true;
            analysis.evidence = ControlFlowEvidence::GuardedPartial;
            analysis.reason =
                "identity-bound-declared-snapshot-candidates";
        } else {
            analysis.reason = "identity-bound-declared-table";
        }
    }
    else if (analysis.reason.empty())
        analysis.reason = "table-entry-rejected";
    return analysis;
}

std::optional<JumpTableAnalysis>
recognize_bounded_relative_jump_table(const katana::io::ExecutableImage& image,
                                       const std::span<const katana::sh4::DisassemblyLine> lines,
                                       const std::size_t dispatch_index,
                                       JumpTableSnapshotCache* const cache) {
    if (dispatch_index < 3u || dispatch_index >= lines.size()) return std::nullopt;
    const auto& dispatch = lines[dispatch_index];
    const auto& load = lines[dispatch_index - 1u];
    const auto& table_base = lines[dispatch_index - 2u];
    if (!contiguous(table_base, load) || !contiguous(load, dispatch) ||
        dispatch.instruction.kind != katana::sh4::InstructionKind::Braf ||
        load.instruction.kind != katana::sh4::InstructionKind::MovWordLoadR0Indexed ||
        load.instruction.destination_register != dispatch.instruction.branch_register ||
        table_base.instruction.kind != katana::sh4::InstructionKind::MoveAddressPcRelative)
        return std::nullopt;

    // GCC-family SH-4 output uses both of these equivalent bounded switch
    // shapes.  The second form avoids the redundant index copy when the
    // scaled selector can remain in its original register:
    //
    //   shll  Rn              shll  Rn
    //   mov   Rn,Rm           mova  table,R0
    //   mova  table,R0        mov.w @(R0,Rn),R0
    //   mov.w @(R0,Rm),Rx     braf  R0
    //   braf  Rx
    //
    // Keep the same dominating bounds proof for both forms.  Recognizing the
    // compact form as a single table is important: treating a case target as
    // an independent function would lose the remaining switch closure.
    std::size_t scale_line_index = dispatch_index - 3u;
    const auto& direct_scale = lines[scale_line_index];
    const bool direct_index =
        contiguous(direct_scale, table_base) &&
        direct_scale.instruction.kind ==
            katana::sh4::InstructionKind::ShiftLogicalLeftOne &&
        direct_scale.instruction.destination_register ==
            load.instruction.source_register &&
        load.instruction.source_register != 0u;
    if (!direct_index) {
        if (dispatch_index < 4u) return std::nullopt;
        const auto& copy_index = lines[dispatch_index - 3u];
        scale_line_index = dispatch_index - 4u;
        const auto& copied_scale = lines[scale_line_index];
        if (!contiguous(copied_scale, copy_index) ||
            !contiguous(copy_index, table_base) ||
            copy_index.instruction.kind !=
                katana::sh4::InstructionKind::MovRegister ||
            copy_index.instruction.destination_register !=
                load.instruction.source_register ||
            copy_index.instruction.destination_register == 0u ||
            copied_scale.instruction.kind !=
                katana::sh4::InstructionKind::ShiftLogicalLeftOne ||
            copied_scale.instruction.destination_register !=
                copy_index.instruction.source_register)
            return std::nullopt;
    }

    const auto entry_count = bounded_entry_count(
        lines,
        image,
        scale_line_index,
        dispatch_index,
        lines[scale_line_index].instruction.destination_register);
    if (!entry_count.has_value()) return std::nullopt;
    const auto table_address = ((table_base.address + 4u) & ~3u) +
                               static_cast<std::uint32_t>(table_base.instruction.displacement);
    const auto table_byte_count = *entry_count * 2u;
    const auto resolved_table = image.resolve_segment_address(table_address, table_byte_count);
    if (!resolved_table.has_value()) return std::nullopt;
    auto analysis = analyze_relative_jump_table(
        image, dispatch.address, *resolved_table, dispatch.address + 4u, *entry_count, cache);
    if (analysis.reason != "table-segment-writable") return analysis;

    const auto dispatch_address = image.resolve_segment_address(dispatch.address, 2u);
    const auto* dispatch_segment = dispatch_address.has_value()
                                       ? image.find_segment(*dispatch_address, 2u)
                                       : nullptr;
    const auto* table_segment = resolved_table.has_value()
                                    ? image.find_segment(*resolved_table, table_byte_count)
                                    : nullptr;
    if (dispatch_segment == nullptr || table_segment == nullptr ||
        !dispatch_segment->permissions.executable || !table_segment->permissions.writable ||
        !snapshot_candidate_source(image, *dispatch_segment) ||
        !snapshot_candidate_source(image, *table_segment))
        return analysis;

    return analyze_relative_jump_table_impl(image,
                                            dispatch.address,
                                            *resolved_table,
                                            dispatch.address + 4u,
                                            *entry_count,
                                            true);
}

std::optional<JumpTableAnalysis>
detail::analyze_snapshot_pointer_candidates(
    const katana::io::ExecutableImage& image,
    const std::uint32_t evidence_address,
    const std::uint32_t table_address,
    const JumpTableDispatchKind dispatch_kind,
    const SnapshotPointerCandidateScanPolicy& policy) {
    if (policy.minimum_entries == 0u ||
        policy.minimum_entries > maximum_jump_table_entries ||
        policy.maximum_scanned_slots < policy.minimum_entries ||
        policy.maximum_scanned_slots > maximum_jump_table_entries ||
        policy.maximum_skipped_slots >= policy.maximum_scanned_slots ||
        policy.maximum_consecutive_skipped_slots > policy.maximum_skipped_slots)
        return std::nullopt;
    const auto resolved_table = image.resolve_segment_address(table_address, 4u);
    if (!resolved_table.has_value() || (*resolved_table & 3u) != 0u) return std::nullopt;
    const auto* table_segment = image.find_segment(*resolved_table, 4u);
    if (table_segment == nullptr || !snapshot_candidate_source(image, *table_segment))
        return std::nullopt;
    const auto table_offset = table_segment->byte_offset(*resolved_table);
    if (!table_offset.has_value() || *table_offset > table_segment->bytes.size())
        return std::nullopt;

    const auto available_entries = (table_segment->bytes.size() - *table_offset) / 4u;
    const auto scan_limit = std::min(available_entries, policy.maximum_scanned_slots);
    JumpTableAnalysis analysis;
    analysis.dispatch_address = evidence_address;
    analysis.table_address = *resolved_table;
    analysis.dispatch_kind = dispatch_kind;
    analysis.encoding = JumpTableEncoding::Absolute32;
    analysis.aot_candidates_only = true;
    analysis.evidence = ControlFlowEvidence::GuardedPartial;
    analysis.entries.reserve(scan_limit);
    std::size_t skipped_slots = 0u;
    std::size_t consecutive_skipped_slots = 0u;
    std::size_t scanned_slots = 0u;
    bool stopped_at_gap = false;
    for (std::size_t index = 0u; index < scan_limit; ++index) {
        scanned_slots = index + 1u;
        const auto offset = *table_offset + index * 4u;
        const auto target = static_cast<std::uint32_t>(
                                katana::io::read_u16_le(table_segment->bytes, offset)) |
                            (static_cast<std::uint32_t>(katana::io::read_u16_le(
                                 table_segment->bytes, offset + 2u))
                             << 16u);
        const auto validation = validate_decode_candidate(image, target);
        if ((policy.treat_null_as_reserved && target == 0u) ||
            !validation.valid() || validation.segment == nullptr ||
            !snapshot_candidate_source(image, *validation.segment)) {
            if (skipped_slots >= policy.maximum_skipped_slots ||
                consecutive_skipped_slots >=
                    policy.maximum_consecutive_skipped_slots) {
                stopped_at_gap = true;
                break;
            }
            ++skipped_slots;
            ++consecutive_skipped_slots;
            continue;
        }
        analysis.entries.push_back({index,
                                    *resolved_table + static_cast<std::uint32_t>(index * 4u),
                                    validation.resolved_address,
                                    true,
                                    "snapshot-absolute-target"});
        consecutive_skipped_slots = 0u;
    }
    analysis.candidate_scan_truncated =
        !stopped_at_gap && scanned_slots == scan_limit &&
        available_entries > scan_limit;
    if (analysis.entries.size() < policy.minimum_entries ||
        (policy.reject_truncated_scan &&
         analysis.entries.size() == policy.maximum_scanned_slots &&
         available_entries > policy.maximum_scanned_slots))
        return std::nullopt;
    analysis.requested_entries = analysis.entries.size();
    analysis.resolved = true;
    analysis.reason = "snapshot-absolute-pointer-candidates";
    return analysis;
}

std::optional<JumpTableAnalysis>
analyze_snapshot_absolute_pointer_candidates(
    const katana::io::ExecutableImage& image,
    const std::uint32_t evidence_address,
    const std::uint32_t table_address,
    const JumpTableDispatchKind dispatch_kind,
    const std::size_t minimum_entries) {
    return detail::analyze_snapshot_pointer_candidates(
        image,
        evidence_address,
        table_address,
        dispatch_kind,
        detail::SnapshotPointerCandidateScanPolicy{
            .minimum_entries = minimum_entries,
            .maximum_scanned_slots = maximum_jump_table_entries,
            .maximum_skipped_slots = 0u,
            .maximum_consecutive_skipped_slots = 0u,
            .treat_null_as_reserved = false,
            .reject_truncated_scan = true,
        });
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

    if (auto displaced = recognize_snapshot_displaced_absolute_pointer_candidates(
            image, lines, dispatch_index);
        displaced.has_value())
        return displaced;

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
    const katana::sh4::DisassemblyLine* base_load = nullptr;
    if (*indexed_load_index != 0u) {
        const auto& adjacent_base = lines[*indexed_load_index - 1u];
        if (contiguous(adjacent_base, indexed_load) &&
            adjacent_base.instruction.kind ==
                katana::sh4::InstructionKind::MovLongLoadPcRelative &&
            adjacent_base.instruction.destination_register ==
                indexed_load.instruction.source_register &&
            indexed_load.instruction.source_register != 0u)
            base_load = &adjacent_base;
    }

    // SH-4 indexed loads add R0 and Rm symmetrically, but the decoder names
    // Rm as the source register.  SDK dispatchers also commonly keep the
    // immutable table base in R0 and the scaled enum in Rm:
    //
    //   mov.l @(disp,pc),r0
    //   shll2 Rm
    //   mov.l @(r0,Rm),Rn
    //   jsr   @Rn
    //
    // Treat that as the same bounded absolute table shape.  Both writers
    // must be the first writes seen on the straight-line backward slice so a
    // stale R0 or an unscaled byte offset cannot manufacture candidates.
    if (base_load == nullptr && indexed_load.instruction.source_register != 0u) {
        const auto index_register = indexed_load.instruction.source_register;
        const katana::sh4::DisassemblyLine* r0_base_load = nullptr;
        bool scaled_index = false;
        bool r0_writer_seen = false;
        bool index_writer_seen = false;
        auto next_address = indexed_load.address;
        for (auto cursor = *indexed_load_index; cursor > 0u;) {
            --cursor;
            const auto& candidate = lines[cursor];
            if (candidate.address > std::numeric_limits<std::uint32_t>::max() - 2u ||
                candidate.address + 2u != next_address)
                break;
            next_address = candidate.address;
            if (candidate.instruction.changes_control_flow()) break;

            const auto writes = general_register_write_mask(candidate.instruction);
            if (!r0_writer_seen && (writes & 1u) != 0u) {
                r0_writer_seen = true;
                if (candidate.instruction.kind ==
                        katana::sh4::InstructionKind::MovLongLoadPcRelative &&
                    candidate.instruction.destination_register == 0u)
                    r0_base_load = &candidate;
            }
            if (!index_writer_seen &&
                (writes & static_cast<std::uint16_t>(1u << index_register)) != 0u) {
                index_writer_seen = true;
                scaled_index =
                    candidate.instruction.kind ==
                        katana::sh4::InstructionKind::ShiftLogicalLeftTwo &&
                    candidate.instruction.destination_register == index_register;
            }
            if (r0_writer_seen && index_writer_seen) break;
        }
        if (r0_base_load != nullptr && scaled_index) base_load = r0_base_load;
    }
    if (base_load == nullptr || base_load->is_delay_slot) return std::nullopt;

    const auto literal_address =
        ((base_load->address + 4u) & ~3u) +
        static_cast<std::uint32_t>(base_load->instruction.displacement);
    const auto resolved_literal = image.resolve_segment_address(literal_address, 4u);
    if (!resolved_literal.has_value()) return std::nullopt;
    const auto* literal_segment = image.find_segment(*resolved_literal, 4u);
    if (literal_segment == nullptr || !snapshot_candidate_source(image, *literal_segment))
        return std::nullopt;

    const auto table_pointer = image.read_u32_le(*resolved_literal);
    return analyze_snapshot_absolute_pointer_candidates(
        image,
        dispatch.address,
        table_pointer,
        dispatch.instruction.kind == katana::sh4::InstructionKind::Jsr
            ? JumpTableDispatchKind::Call
            : JumpTableDispatchKind::Jump,
        2u);
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
    case JumpTableEncoding::SignedRelative32:
        return "signed-relative32";
    }
    return "unknown";
}

} // namespace katana::analysis
