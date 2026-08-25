#pragma once

#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::uint32_t resolved_address = 0u;

    [[nodiscard]] bool valid() const noexcept {
        return status == CodeAddressStatus::Valid;
    }
};

// Positive proof that one SH-4 instruction word is the architectural delay
// slot owned by the immediately preceding instruction word.  This primitive
// deliberately proves only physical ownership.  It does not authorize the
// slot as an independent function/root entry; callers must retain their own
// normal-entry provenance policy for that separate context.
struct Sh4PhysicalDelaySlotProof {
    std::uint32_t entry_address = 0u;
    std::uint32_t owner_address = 0u;
};

[[nodiscard]] std::optional<Sh4PhysicalDelaySlotProof>
prove_sh4_physical_delay_slot(const katana::io::ExecutableImage& image,
                              std::uint32_t address) noexcept;

// Equivalent proof for an already authenticated contiguous byte extent.  The
// extent base is the source/runtime address represented by bytes[0].
[[nodiscard]] std::optional<Sh4PhysicalDelaySlotProof>
prove_sh4_physical_delay_slot(std::span<const std::uint8_t> bytes,
                              std::uint32_t extent_address,
                              std::uint32_t address) noexcept;

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
