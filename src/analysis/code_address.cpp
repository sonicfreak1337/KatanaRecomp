#include "katana/analysis/code_address.hpp"

#include <algorithm>
#include <cstdint>

namespace katana::analysis {

CodeAddressValidation validate_decode_candidate(const katana::io::ExecutableImage& image,
                                                const std::uint32_t address,
                                                const std::size_t width) noexcept {
    if ((address & 1u) != 0u) {
        return {CodeAddressStatus::OddAddress, nullptr};
    }
    const katana::io::ImageSegment* containing = nullptr;
    for (const auto& segment : image.segments()) {
        const auto begin = static_cast<std::uint64_t>(address);
        if (begin >= segment.virtual_address && begin < segment.end_address()) {
            containing = &segment;
            break;
        }
    }
    if (containing == nullptr || !containing->contains(address, width)) {
        return {CodeAddressStatus::OutsideSegments, containing};
    }
    if (containing->kind != katana::io::SegmentKind::Code) {
        return {CodeAddressStatus::NotCodeSegment, containing};
    }
    if (!containing->permissions.executable) {
        return {CodeAddressStatus::NotExecutableSegment, containing};
    }
    const auto offset = containing->byte_offset(address);
    if (!offset.has_value() || containing->bytes.size() < width ||
        *offset > containing->bytes.size() - width) {
        return {CodeAddressStatus::OutsideCommittedData, containing};
    }
    return {CodeAddressStatus::Valid, containing};
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
