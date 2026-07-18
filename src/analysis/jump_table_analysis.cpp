#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/code_address.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"

#include <limits>
#include <stdexcept>

namespace katana::analysis {
namespace {

constexpr std::size_t maximum_jump_table_entries = 4096u;

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

JumpTableAnalysis analyze_jump_table(const katana::io::ExecutableImage& image,
                                     const std::uint32_t dispatch_address,
                                     const std::uint32_t table_address,
                                     const std::size_t entry_count) {
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
                                              const std::size_t entry_count) {
    return analyze_relative_jump_table_impl(
        image, dispatch_address, table_address, target_base, entry_count);
}

std::optional<JumpTableAnalysis>
recognize_bounded_relative_jump_table(const katana::io::ExecutableImage& image,
                                      const std::span<const katana::sh4::DisassemblyLine> lines,
                                      const std::size_t dispatch_index) {
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
    return analyze_relative_jump_table_impl(
        image, dispatch.address, table_address, dispatch.address + 4u, *entry_count);
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
