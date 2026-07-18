#pragma once

#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace katana::analysis {

enum class CodeAddressStatus {
    Valid,
    OddAddress,
    OutsideSegments,
    NotCodeSegment,
    NotExecutableSegment,
    OutsideCommittedData
};

struct CodeAddressValidation {
    CodeAddressStatus status = CodeAddressStatus::OutsideSegments;
    const katana::io::ImageSegment* segment = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return status == CodeAddressStatus::Valid;
    }
};

[[nodiscard]] CodeAddressValidation
validate_decode_candidate(const katana::io::ExecutableImage& image,
                          std::uint32_t address,
                          std::size_t width = 2u) noexcept;

[[nodiscard]] bool proven_instruction_boundary(std::span<const std::uint32_t> proven_addresses,
                                               std::uint32_t address) noexcept;

[[nodiscard]] CodeAddressValidation
validate_committed_code_address(const katana::io::ExecutableImage& image,
                                std::uint32_t address,
                                std::size_t width = 2u) noexcept;

[[nodiscard]] const char* code_address_status_name(CodeAddressStatus status) noexcept;

} // namespace katana::analysis
