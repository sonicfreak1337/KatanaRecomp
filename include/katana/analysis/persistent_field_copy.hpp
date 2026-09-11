#pragma once

#include <cstdint>
#include <compare>

namespace katana::analysis {

// Positive source-bound data-flow evidence: this function can copy one
// unchanged incoming record field to non-stack memory. The instruction pair
// identifies the exact load and store; this is neither a callback ABI nor an
// exhaustive memory effect. Consumers must bind concrete source bytes and
// independently validate any executable candidate found in that field.
struct PersistentFieldCopyContract final {
    std::uint32_t function_address = 0u;
    std::uint32_t load_instruction_address = 0u;
    std::uint32_t store_instruction_address = 0u;
    std::int32_t displacement = 0;
    std::uint8_t argument = 0u; // incoming r4..r7
    std::uint8_t width = 0u;

    bool operator==(const PersistentFieldCopyContract&) const = default;
    auto operator<=>(const PersistentFieldCopyContract&) const = default;
};

} // namespace katana::analysis
