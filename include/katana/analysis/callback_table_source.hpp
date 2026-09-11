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
    // One source-bound resident table word passed to an exact callback sink.
    // Null holes and an unbounded selector never establish table completeness.
    ResidentCallbackCell = 3u,
    // A source-bound resident cell publishes a header through an exact global
    // cell. Its consumer reads the record pointer and a callback field. Neither
    // a record count nor an exhaustive selector domain is implied.
    PublishedHeaderRecords = 4u,
};

[[nodiscard]] constexpr bool valid_callback_table_source(
    CallbackRecordTableSource kind, std::uint8_t argument,
    std::int32_t header_displacement,
    std::uint32_t vector_address = 0u,
    std::uint32_t resident_cell_address = 0u,
    std::uint32_t resident_target_address = 0u,
    std::uint32_t record_stride = 0u) noexcept {
    if (kind == CallbackRecordTableSource::ResidentCallbackCell)
        return argument == 0u && header_displacement == 0 &&
            vector_address >= 0x8c000000u && vector_address < 0x90000000u &&
            (vector_address & 3u) == 0u &&
            resident_cell_address >= vector_address && resident_cell_address < 0x90000000u &&
            record_stride >= 4u && record_stride <= 256u && (record_stride & 3u) == 0u &&
            (resident_cell_address - vector_address) % record_stride == 0u &&
            (resident_cell_address - vector_address) / record_stride < 256u &&
            resident_target_address >= 0x8c000000u && resident_target_address < 0x90000000u &&
            (resident_target_address & 1u) == 0u;
    if (kind == CallbackRecordTableSource::PublishedHeaderRecords) {
        const auto p1_word = [](std::uint32_t address) {
            return address >= 0x8c000000u && address < 0x90000000u &&
                (address & 3u) == 0u;
        };
        return argument == 0u && p1_word(vector_address) &&
            p1_word(resident_cell_address) && p1_word(resident_target_address) &&
            header_displacement >= 0 && header_displacement <= 4096 &&
            (header_displacement & 3) == 0 && record_stride >= 4u &&
            record_stride <= 256u && (record_stride & 3u) == 0u;
    }
    if (resident_cell_address != 0u || resident_target_address != 0u) return false;
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
