#include "katana/analysis/runtime_code_copy_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace katana::analysis {
namespace {

constexpr std::uint32_t maximum_copy_bytes = 1024u * 1024u;
constexpr std::uint32_t maximum_patch_lookback_bytes = 4096u;

bool contiguous(const katana::sh4::DisassemblyLine& left,
                const katana::sh4::DisassemblyLine& right) noexcept {
    return left.address <= std::numeric_limits<std::uint32_t>::max() - 2u &&
           left.address + 2u == right.address;
}

std::optional<std::uint32_t> pc_relative_literal_address(const katana::sh4::DisassemblyLine& line,
                                                         const std::size_t width) noexcept {
    const auto base = width == 4u ? (static_cast<std::uint64_t>(line.address) + 4u) & ~3ull
                                  : static_cast<std::uint64_t>(line.address) + 4u;
    const auto displacement = static_cast<std::uint64_t>(line.instruction.displacement);
    if (base + displacement > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(base + displacement);
}

bool snapshot_segment(const katana::io::ImageSegment& segment) noexcept {
    return segment.permissions.readable &&
           segment.load_phase == katana::io::ImageLoadPhase::Initial &&
           segment.source_kind != katana::io::ImageSourceKind::RuntimeMemory;
}

std::optional<std::uint16_t> read_snapshot_u16(const katana::io::ExecutableImage& image,
                                               const std::uint32_t address) {
    const auto resolved = image.resolve_segment_address(address, 2u);
    if (!resolved.has_value()) return std::nullopt;
    const auto* segment = image.find_segment(*resolved, 2u);
    if (segment == nullptr || !snapshot_segment(*segment)) return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() || segment->bytes.size() < 2u || *offset > segment->bytes.size() - 2u)
        return std::nullopt;
    return katana::io::read_u16_le(segment->bytes, *offset);
}

std::optional<std::uint32_t> read_snapshot_u32(const katana::io::ExecutableImage& image,
                                               const std::uint32_t address) {
    const auto resolved = image.resolve_segment_address(address, 4u);
    if (!resolved.has_value()) return std::nullopt;
    const auto* segment = image.find_segment(*resolved, 4u);
    if (segment == nullptr || !snapshot_segment(*segment)) return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() || segment->bytes.size() < 4u || *offset > segment->bytes.size() - 4u)
        return std::nullopt;
    const auto low = static_cast<std::uint32_t>(segment->bytes[*offset]);
    const auto byte1 = static_cast<std::uint32_t>(segment->bytes[*offset + 1u]);
    const auto byte2 = static_cast<std::uint32_t>(segment->bytes[*offset + 2u]);
    const auto high = static_cast<std::uint32_t>(segment->bytes[*offset + 3u]);
    return low | (byte1 << 8u) | (byte2 << 16u) | (high << 24u);
}

bool pc_relative_snapshot_source(const std::string& source) {
    constexpr std::array names{"pc-relative-literal",
                               "guarded-writable-pc-relative-literal",
                               "entry-snapshot-pc-relative-literal"};
    return std::find(names.begin(), names.end(), source) != names.end();
}

bool address_is_instruction(const std::span<const katana::sh4::DisassemblyLine> instructions,
                            const std::uint32_t address) {
    const auto found = std::lower_bound(
        instructions.begin(),
        instructions.end(),
        address,
        [](const auto& line, const std::uint32_t candidate) { return line.address < candidate; });
    return found != instructions.end() && found->address == address;
}

bool long_slot_is_not_instruction(const std::span<const katana::sh4::DisassemblyLine> instructions,
                                  const std::uint32_t address) {
    return address <= std::numeric_limits<std::uint32_t>::max() - 2u &&
           !address_is_instruction(instructions, address) &&
           !address_is_instruction(instructions, address + 2u);
}

std::optional<std::uint32_t> canonical_data_address(const katana::io::ExecutableImage& image,
                                                    const std::uint32_t address,
                                                    const std::size_t width) noexcept {
    return image.resolve_segment_address(address, width);
}

bool copied_range_contains(const RuntimeCodeCopy& copy,
                           const std::uint32_t address,
                           const std::size_t width) noexcept {
    const auto begin = static_cast<std::uint64_t>(copy.source_begin);
    const auto end = begin + copy.source_byte_count;
    const auto candidate_begin = static_cast<std::uint64_t>(address);
    const auto candidate_end = candidate_begin + width;
    return candidate_begin >= begin && candidate_end >= candidate_begin && candidate_end <= end;
}

std::vector<RuntimeCodePatchCandidate>
find_patch_candidates(const katana::io::ExecutableImage& image,
                      const std::span<const katana::sh4::DisassemblyLine> instructions,
                      const std::span<const ConstantTraceEntry> trace,
                      const RuntimeCodeCopy& copy) {
    std::unordered_set<std::uint32_t> template_literal_slots;
    for (const auto& line : instructions) {
        if (!copied_range_contains(copy, line.address, 2u) ||
            line.instruction.kind != katana::sh4::InstructionKind::MovLongLoadPcRelative)
            continue;
        const auto raw_slot = pc_relative_literal_address(line, 4u);
        if (!raw_slot.has_value()) continue;
        const auto slot = canonical_data_address(image, *raw_slot, 4u);
        if (!slot.has_value() || !copied_range_contains(copy, *slot, 4u) ||
            !long_slot_is_not_instruction(instructions, *slot))
            continue;
        template_literal_slots.insert(*slot);
    }
    if (template_literal_slots.empty()) return {};

    std::vector<RuntimeCodePatchCandidate> result;
    const auto scan_begin = copy.setup_address > maximum_patch_lookback_bytes
                                ? copy.setup_address - maximum_patch_lookback_bytes
                                : 0u;
    for (std::size_t index = 0u; index < instructions.size() && index < trace.size(); ++index) {
        const auto& line = instructions[index];
        const auto& operation = line.instruction;
        if (line.address < scan_begin || line.address >= copy.setup_address ||
            operation.kind != katana::sh4::InstructionKind::MovLongStore)
            continue;
        const auto destination_register = operation.destination_register;
        const auto source_register = operation.source_register;
        const auto& before = trace[index].before;
        if (!before.registers[destination_register].has_value() ||
            !before.registers[source_register].has_value() ||
            !pc_relative_snapshot_source(before.sources[destination_register]) ||
            !pc_relative_snapshot_source(before.sources[source_register]))
            continue;

        const auto slot =
            canonical_data_address(image, *before.registers[destination_register], 4u);
        if (!slot.has_value() || !template_literal_slots.contains(*slot)) continue;
        const auto target =
            validate_decode_candidate(image, *before.registers[source_register], 2u);
        if (!target.valid()) continue;
        result.push_back({line.address,
                          *slot,
                          *before.registers[source_register],
                          target.resolved_address});
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.slot_address,
                        left.live_value,
                        left.target_address,
                        left.store_instruction_address) <
               std::tie(right.slot_address,
                        right.live_value,
                        right.target_address,
                        right.store_instruction_address);
    });
    result.erase(std::unique(result.begin(),
                             result.end(),
                             [](const auto& left, const auto& right) {
                                 return left.slot_address == right.slot_address &&
                                        left.live_value == right.live_value &&
                                        left.target_address == right.target_address;
                             }),
                 result.end());
    return result;
}

std::optional<RuntimeCodeCopy>
recognize_copy_loop(const katana::io::ExecutableImage& image,
                    const std::span<const katana::sh4::DisassemblyLine> instructions,
                    const std::size_t loop_index) {
    using K = katana::sh4::InstructionKind;
    constexpr std::size_t setup_instruction_count = 5u;
    constexpr std::size_t loop_instruction_count = 6u;
    if (loop_index < setup_instruction_count ||
        instructions.size() - loop_index < loop_instruction_count)
        return std::nullopt;

    const auto& vbr = instructions[loop_index - 5u];
    const auto& source = instructions[loop_index - 4u];
    const auto& delta = instructions[loop_index - 3u];
    const auto& precheck = instructions[loop_index - 2u];
    const auto& destination_add = instructions[loop_index - 1u];
    const auto& load = instructions[loop_index];
    const auto& store = instructions[loop_index + 1u];
    const auto& destination_increment = instructions[loop_index + 2u];
    const auto& end = instructions[loop_index + 3u];
    const auto& compare = instructions[loop_index + 4u];
    const auto& branch = instructions[loop_index + 5u];

    for (auto index = loop_index - setup_instruction_count;
         index < loop_index + loop_instruction_count - 1u;
         ++index) {
        if (!contiguous(instructions[index], instructions[index + 1u])) return std::nullopt;
    }

    const auto destination_register = store.instruction.destination_register;
    const auto source_register = load.instruction.source_register;
    const auto temporary_register = load.instruction.destination_register;
    const auto end_register = end.instruction.destination_register;
    const auto delta_register = delta.instruction.destination_register;
    if (vbr.instruction.kind != K::StoreSpecialRegister ||
        vbr.instruction.special_register != katana::sh4::SpecialRegister::Vbr ||
        vbr.instruction.destination_register != destination_register ||
        source.instruction.kind != K::MovLongLoadPcRelative ||
        source.instruction.destination_register != source_register ||
        delta.instruction.kind != K::MovWordLoadPcRelative || precheck.instruction.kind != K::Bra ||
        !precheck.target_address.has_value() || *precheck.target_address != end.address ||
        destination_add.instruction.kind != K::AddRegister ||
        destination_add.instruction.destination_register != destination_register ||
        destination_add.instruction.source_register != delta_register ||
        load.instruction.kind != K::MovLongLoadPostIncrement ||
        temporary_register == source_register || store.instruction.kind != K::MovLongStore ||
        store.instruction.source_register != temporary_register ||
        destination_increment.instruction.kind != K::AddImmediate ||
        destination_increment.instruction.destination_register != destination_register ||
        destination_increment.instruction.immediate != 4 ||
        end.instruction.kind != K::MovLongLoadPcRelative ||
        compare.instruction.kind != K::CompareHigher ||
        compare.instruction.destination_register != source_register ||
        compare.instruction.source_register != end_register || branch.instruction.kind != K::Bf ||
        !branch.target_address.has_value() || *branch.target_address != load.address)
        return std::nullopt;

    const auto source_literal = pc_relative_literal_address(source, 4u);
    const auto end_literal = pc_relative_literal_address(end, 4u);
    const auto delta_literal = pc_relative_literal_address(delta, 2u);
    if (!source_literal.has_value() || !end_literal.has_value() || !delta_literal.has_value() ||
        !long_slot_is_not_instruction(instructions, *source_literal) ||
        !long_slot_is_not_instruction(instructions, *end_literal) ||
        address_is_instruction(instructions, *delta_literal))
        return std::nullopt;

    const auto raw_source_begin = read_snapshot_u32(image, *source_literal);
    const auto raw_source_end = read_snapshot_u32(image, *end_literal);
    const auto raw_delta = read_snapshot_u16(image, *delta_literal);
    if (!raw_source_begin.has_value() || !raw_source_end.has_value() || !raw_delta.has_value())
        return std::nullopt;

    const auto begin_validation = validate_decode_candidate(image, *raw_source_begin, 4u);
    const auto end_validation = validate_decode_candidate(image, *raw_source_end, 4u);
    if (!begin_validation.valid() || !end_validation.valid() ||
        begin_validation.segment != end_validation.segment)
        return std::nullopt;
    const auto source_begin = begin_validation.resolved_address;
    const auto source_end = end_validation.resolved_address;
    if ((source_begin & 3u) != 0u || (source_end & 3u) != 0u || source_end < source_begin)
        return std::nullopt;
    const auto span = static_cast<std::uint64_t>(source_end) - source_begin + 4u;
    if (span == 0u || span > maximum_copy_bytes ||
        !begin_validation.segment->contains(source_begin, static_cast<std::size_t>(span)))
        return std::nullopt;

    RuntimeCodeCopy copy;
    copy.setup_address = vbr.address;
    copy.loop_address = load.address;
    copy.source_begin = source_begin;
    copy.source_end_inclusive = source_end;
    copy.source_byte_count = static_cast<std::uint32_t>(span);
    copy.destination_vbr_delta = static_cast<std::int16_t>(*raw_delta);
    copy.reason = "bounded-vbr-runtime-code-copy";
    return copy;
}

} // namespace

RuntimeCodeCopyAnalysis
analyze_runtime_code_copies(const katana::io::ExecutableImage& image,
                            const std::span<const katana::sh4::DisassemblyLine> instructions) {
    RuntimeCodeCopyAnalysis analysis;
    for (std::size_t index = 0u; index < instructions.size(); ++index) {
        auto copy = recognize_copy_loop(image, instructions, index);
        if (!copy.has_value()) continue;
        analysis.copies.push_back(std::move(*copy));
    }
    if (analysis.copies.empty()) return analysis;

    const auto trace = propagate_local_constants(instructions, image);
    for (auto& copy : analysis.copies)
        copy.patch_candidates = find_patch_candidates(image, instructions, trace, copy);
    std::sort(
        analysis.copies.begin(), analysis.copies.end(), [](const auto& left, const auto& right) {
            if (left.setup_address != right.setup_address)
                return left.setup_address < right.setup_address;
            return left.loop_address < right.loop_address;
        });
    analysis.copies.erase(std::unique(analysis.copies.begin(),
                                      analysis.copies.end(),
                                      [](const auto& left, const auto& right) {
                                          return left.setup_address == right.setup_address &&
                                                 left.loop_address == right.loop_address;
                                      }),
                          analysis.copies.end());
    return analysis;
}

} // namespace katana::analysis
