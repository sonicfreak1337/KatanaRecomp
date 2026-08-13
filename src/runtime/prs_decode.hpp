#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace katana::detail {

enum class PrsDecodeFailure : std::uint8_t {
    InvalidLimits,
    InvalidInput,
    CompressedInputLimit,
    DecompressedOutputLimit,
};

class PrsDecodeError final : public std::runtime_error {
  public:
    PrsDecodeError(const PrsDecodeFailure failure,
                   const std::size_t source_offset,
                   const std::string_view operation)
        : std::runtime_error(std::string(operation)),
          failure_(failure), source_offset_(source_offset),
          operation_(operation) {}

    [[nodiscard]] PrsDecodeFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] std::size_t source_offset() const noexcept {
        return source_offset_;
    }
    [[nodiscard]] std::string_view operation() const noexcept {
        return operation_;
    }

  private:
    PrsDecodeFailure failure_;
    std::size_t source_offset_;
    std::string_view operation_;
};

namespace prs_decode_detail {

[[noreturn]] inline void fail(const PrsDecodeFailure failure,
                              const std::size_t source_offset,
                              const std::string_view operation) {
    throw PrsDecodeError(failure, source_offset, operation);
}

class Reader final {
  public:
    explicit Reader(const std::span<const std::uint8_t> source)
        : source_(source) {}

    [[nodiscard]] bool exhausted() const noexcept {
        return offset_ == source_.size();
    }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    [[nodiscard]] std::uint8_t read_byte() {
        if (exhausted())
            fail(PrsDecodeFailure::InvalidInput, offset_,
                 "prs-truncated-token");
        return source_[offset_++];
    }

    [[nodiscard]] bool read_bit() {
        if (remaining_control_bits_ == 0u) {
            control_ = read_byte();
            remaining_control_bits_ = 8u;
        }
        const bool result = (control_ & 1u) != 0u;
        control_ >>= 1u;
        --remaining_control_bits_;
        return result;
    }

  private:
    std::span<const std::uint8_t> source_;
    std::size_t offset_ = 0u;
    std::uint8_t control_ = 0u;
    std::uint8_t remaining_control_bits_ = 0u;
};

inline void append_byte(std::vector<std::uint8_t>& output,
                        const std::uint8_t value,
                        const std::size_t maximum_output_bytes,
                        const std::size_t source_offset) {
    if (output.size() == maximum_output_bytes)
        fail(PrsDecodeFailure::DecompressedOutputLimit, source_offset,
             "prs-output-limit");
    output.push_back(value);
}

inline void append_copy(std::vector<std::uint8_t>& output,
                        const std::size_t distance,
                        const std::size_t length,
                        const std::size_t maximum_output_bytes,
                        const std::size_t source_offset) {
    if (distance == 0u || distance > output.size())
        fail(PrsDecodeFailure::InvalidInput, source_offset,
             "prs-invalid-back-reference");
    if (length > maximum_output_bytes - output.size())
        fail(PrsDecodeFailure::DecompressedOutputLimit, source_offset,
             "prs-output-limit");
    for (std::size_t index = 0u; index < length; ++index)
        output.push_back(output[output.size() - distance]);
}

} // namespace prs_decode_detail

// Legacy Sega PRS uses a least-significant-bit-first control stream. A valid
// stream must terminate with its zero-offset long-copy token exactly at the
// end of the supplied source span. The decoder owns no platform or title
// state, so runtime asset loading and static AOT discovery share one contract.
[[nodiscard]] inline std::vector<std::uint8_t> decompress_sega_prs(
    const std::span<const std::uint8_t> source,
    const std::size_t maximum_compressed_bytes,
    const std::size_t maximum_decompressed_bytes) {
    using namespace prs_decode_detail;
    if (maximum_compressed_bytes == 0u ||
        maximum_decompressed_bytes == 0u)
        fail(PrsDecodeFailure::InvalidLimits, 0u, "prs-limits");
    if (source.empty())
        fail(PrsDecodeFailure::InvalidInput, 0u, "prs-empty");
    if (source.size() > maximum_compressed_bytes)
        fail(PrsDecodeFailure::CompressedInputLimit, 0u,
             "prs-input-limit");

    Reader reader(source);
    std::vector<std::uint8_t> output;
    output.reserve(std::min(source.size(), maximum_decompressed_bytes));

    for (;;) {
        if (reader.exhausted())
            fail(PrsDecodeFailure::InvalidInput, reader.offset(),
                 "prs-missing-terminator");

        if (reader.read_bit()) {
            append_byte(output, reader.read_byte(),
                        maximum_decompressed_bytes, reader.offset());
            continue;
        }

        if (!reader.read_bit()) {
            const std::size_t first_length_bit =
                reader.read_bit() ? 1u : 0u;
            const std::size_t second_length_bit =
                reader.read_bit() ? 1u : 0u;
            const std::size_t length =
                ((first_length_bit << 1u) | second_length_bit) + 2u;
            const auto encoded_offset = reader.read_byte();
            append_copy(output, 256u - encoded_offset, length,
                        maximum_decompressed_bytes, reader.offset());
            continue;
        }

        const auto low = reader.read_byte();
        const auto high = reader.read_byte();
        const auto encoded = static_cast<std::uint16_t>(low) |
                             static_cast<std::uint16_t>(high << 8u);
        if (encoded == 0u) {
            if (!reader.exhausted())
                fail(PrsDecodeFailure::InvalidInput, reader.offset(),
                     "prs-data-after-terminator");
            return output;
        }

        const std::size_t distance = 8'192u - (encoded >> 3u);
        std::size_t length = encoded & 0x7u;
        if (length == 0u)
            length = static_cast<std::size_t>(reader.read_byte()) + 1u;
        else
            length += 2u;
        append_copy(output, distance, length, maximum_decompressed_bytes,
                    reader.offset());
    }
}

} // namespace katana::detail
