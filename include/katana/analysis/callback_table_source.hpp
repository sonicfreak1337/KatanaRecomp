#pragma once

#include <cstdint>

namespace katana::analysis {

// The source and termination rule are established by the consumer's code,
// never inferred from pointer-shaped words in a candidate data image.
enum class CallbackRecordTableSource : std::uint8_t {
    HeaderCount = 0u,
    DirectSentinelArgument = 1u,
    // A concrete canonical runtime vector observed through an identity-bound
    // consumer. This is positive RuntimeOnly inventory, never a proof that
    // the surrounding record or selector domain is complete.
    StaticVectorAddress = 2u,
};

[[nodiscard]] constexpr bool valid_callback_table_source(
    CallbackRecordTableSource kind, std::uint8_t argument,
    std::int32_t header_displacement,
    std::uint32_t vector_address = 0u) noexcept {
    if (kind == CallbackRecordTableSource::DirectSentinelArgument)
        return argument < 4u && header_displacement == 0 &&
            vector_address == 0u;
    if (kind == CallbackRecordTableSource::HeaderCount)
        return argument == 0u && vector_address == 0u &&
            header_displacement >= 4 && header_displacement <= 4096 &&
            (header_displacement & 3) == 0;
    return kind == CallbackRecordTableSource::StaticVectorAddress &&
        argument == 0u && header_displacement == 0 &&
        vector_address >= 0x8c000000u && vector_address < 0x90000000u &&
        (vector_address & 3u) == 0u;
}

} // namespace katana::analysis
