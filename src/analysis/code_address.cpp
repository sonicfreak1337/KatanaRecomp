#include "katana/analysis/code_address.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <cstdint>

namespace katana::analysis {

namespace {

std::optional<Sh4PhysicalDelaySlotProof>
prove_sh4_physical_delay_slot_in_extent(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t extent_address,
    const std::uint32_t address) noexcept {
    if ((extent_address & 1u) != 0u || (address & 1u) != 0u ||
        address < extent_address)
        return std::nullopt;
    const auto entry_offset = static_cast<std::uint64_t>(address) -
                              extent_address;
    if (entry_offset < sizeof(std::uint16_t) ||
        entry_offset + sizeof(std::uint16_t) > bytes.size())
        return std::nullopt;
    const auto owner_offset = static_cast<std::size_t>(
        entry_offset - sizeof(std::uint16_t));
    const auto owner = katana::sh4::decode(
        katana::io::read_u16_le(bytes, owner_offset));
    if (!owner.is_known() || !owner.has_delay_slot) return std::nullopt;
    return Sh4PhysicalDelaySlotProof{
        address,
        static_cast<std::uint32_t>(address - sizeof(std::uint16_t))};
}

} // namespace

std::optional<Sh4PhysicalDelaySlotProof>
prove_sh4_physical_delay_slot(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address) noexcept {
    const auto entry = validate_decode_candidate(
        image, address, sizeof(std::uint16_t));
    if (!entry.valid() || entry.segment == nullptr ||
        entry.resolved_address < sizeof(std::uint16_t))
        return std::nullopt;
    const auto owner_address = static_cast<std::uint32_t>(
        entry.resolved_address - sizeof(std::uint16_t));
    // Numerically adjacent words from distinct mapped segments/generations do
    // not form one architectural owner/slot pair.  The entry was already
    // canonicalized above; do not resolve its preceding source word through a
    // second, potentially unrelated runtime alias.
    if (!entry.segment->contains(owner_address, sizeof(std::uint16_t)))
        return std::nullopt;
    const auto owner_offset = entry.segment->byte_offset(owner_address);
    if (!owner_offset.has_value() ||
        *owner_offset > entry.segment->bytes.size() ||
        entry.segment->bytes.size() - *owner_offset <
            sizeof(std::uint16_t))
        return std::nullopt;
    const auto decoded = katana::sh4::decode(
        katana::io::read_u16_le(entry.segment->bytes, *owner_offset));
    if (!decoded.is_known() || !decoded.has_delay_slot)
        return std::nullopt;
    return Sh4PhysicalDelaySlotProof{
        entry.resolved_address, owner_address};
}

std::optional<Sh4PhysicalDelaySlotProof>
prove_sh4_physical_delay_slot(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t extent_address,
    const std::uint32_t address) noexcept {
    return prove_sh4_physical_delay_slot_in_extent(
        bytes, extent_address, address);
}

CodeAddressValidation validate_decode_candidate(const katana::io::ExecutableImage& image,
                                                const std::uint32_t address,
                                                const std::size_t width) noexcept {
    if ((address & 1u) != 0u) {
        return {CodeAddressStatus::OddAddress, nullptr, address};
    }
    const auto resolved = image.resolve_segment_address(address, width);
    if (!resolved.has_value()) return {CodeAddressStatus::OutsideSegments, nullptr, address};
    const auto resolved_address = *resolved;
    const katana::io::ImageSegment* containing = nullptr;
    for (const auto& segment : image.segments()) {
        const auto begin = static_cast<std::uint64_t>(resolved_address);
        if (begin >= segment.virtual_address && begin < segment.end_address()) {
            containing = &segment;
            break;
        }
    }
    if (containing == nullptr || !containing->contains(resolved_address, width)) {
        return {CodeAddressStatus::OutsideSegments, containing, resolved_address};
    }
    if (containing->kind != katana::io::SegmentKind::Code &&
        containing->kind != katana::io::SegmentKind::Mixed) {
        return {CodeAddressStatus::NotCodeSegment, containing, resolved_address};
    }
    if (!containing->permissions.executable) {
        return {CodeAddressStatus::NotExecutableSegment, containing, resolved_address};
    }
    const auto offset = containing->byte_offset(resolved_address);
    if (!offset.has_value() || containing->bytes.size() < width ||
        *offset > containing->bytes.size() - width) {
        return {CodeAddressStatus::OutsideCommittedData, containing, resolved_address};
    }
    return {CodeAddressStatus::Valid, containing, resolved_address};
}

bool proven_instruction_boundary(const std::span<const std::uint32_t> proven_addresses,
                                 const std::uint32_t address) noexcept {
    return std::binary_search(proven_addresses.begin(), proven_addresses.end(), address);
}

CodeAddressValidation validate_committed_code_address(const katana::io::ExecutableImage& image,
                                                      const std::uint32_t address,
                                                      const std::size_t width) noexcept {
    return validate_decode_candidate(image, address, width);
}

const char* code_address_status_name(const CodeAddressStatus status) noexcept {
    switch (status) {
    case CodeAddressStatus::Valid:
        return "valid";
    case CodeAddressStatus::OddAddress:
        return "odd-address";
    case CodeAddressStatus::OutsideSegments:
        return "outside-segments";
    case CodeAddressStatus::NotCodeSegment:
        return "not-code-segment";
    case CodeAddressStatus::NotExecutableSegment:
        return "not-executable-segment";
    case CodeAddressStatus::OutsideCommittedData:
        return "outside-committed-data";
    }
    return "unknown";
}

} // namespace katana::analysis
