#include "katana/runtime/aica.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>

namespace katana::runtime {

std::size_t AicaRegisterFile::width_bytes(const MemoryAccessWidth width) noexcept {
    return static_cast<std::size_t>(width);
}

void AicaRegisterFile::check(const std::uint32_t offset, const MemoryAccessWidth width) const {
    const auto bytes = width_bytes(width);
    if (static_cast<std::size_t>(offset) + bytes > registers_.size()) {
        throw std::out_of_range("AICA-Registerzugriff liegt ausserhalb des Registerfensters.");
    }
}

std::uint32_t AicaRegisterFile::read(
    const std::uint32_t offset,
    const MemoryAccessWidth width
) const {
    check(offset, width);
    std::uint32_t result = 0u;
    for (std::size_t index = 0u; index < width_bytes(width); ++index) {
        result |= static_cast<std::uint32_t>(registers_[offset + index]) << (index * 8u);
    }
    return result;
}

void AicaRegisterFile::write(
    const std::uint32_t offset,
    const std::uint32_t value,
    const MemoryAccessWidth width
) {
    check(offset, width);
    for (std::size_t index = 0u; index < width_bytes(width); ++index) {
        registers_[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
    ++writes_;
}

void AicaRegisterFile::reset() noexcept {
    registers_.fill(0u);
    writes_ = 0u;
}

std::uint64_t AicaRegisterFile::write_count() const noexcept { return writes_; }

AicaSampleDecoder::AicaSampleDecoder(const AicaSampleFormat format) noexcept : format_(format) {}

std::int16_t AicaSampleDecoder::decode_adpcm_nibble(const std::uint8_t nibble) noexcept {
    static constexpr std::array<std::int32_t, 8> step_scale = {
        230, 230, 230, 230, 307, 409, 512, 614
    };
    const auto magnitude = static_cast<std::int32_t>(nibble & 0x7u);
    const auto delta = ((magnitude * 2 + 1) * step_) >> 3;
    predictor_ += (nibble & 0x8u) != 0u ? -delta : delta;
    predictor_ = std::clamp(predictor_, -32768, 32767);
    step_ = (step_ * step_scale[static_cast<std::size_t>(magnitude)]) >> 8;
    step_ = std::clamp(step_, 127, 24576);
    return static_cast<std::int16_t>(predictor_);
}

std::vector<std::int16_t> AicaSampleDecoder::decode(
    const std::span<const std::uint8_t> source,
    const std::size_t sample_count
) {
    if (format_ == AicaSampleFormat::Pcm16 &&
        sample_count > std::numeric_limits<std::size_t>::max() / 2u) {
        throw std::out_of_range("AICA-Samplezahl ist zu gross.");
    }
    const std::size_t required = format_ == AicaSampleFormat::Pcm16
        ? sample_count * 2u
        : format_ == AicaSampleFormat::Pcm8 ? sample_count : sample_count / 2u + sample_count % 2u;
    if (source.size() < required) {
        throw std::out_of_range("AICA-Sampledaten sind fuer die angeforderte Samplezahl zu kurz.");
    }
    std::vector<std::int16_t> result;
    result.reserve(sample_count);
    for (std::size_t index = 0u; index < sample_count; ++index) {
        if (format_ == AicaSampleFormat::Pcm16) {
            const auto raw = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(source[index * 2u]) |
                static_cast<std::uint16_t>(source[index * 2u + 1u] << 8u)
            );
            result.push_back(std::bit_cast<std::int16_t>(raw));
        } else if (format_ == AicaSampleFormat::Pcm8) {
            const auto sample = std::bit_cast<std::int8_t>(source[index]);
            result.push_back(static_cast<std::int16_t>(static_cast<std::int16_t>(sample) * 256));
        } else {
            const auto packed = source[index / 2u];
            const auto nibble = static_cast<std::uint8_t>((index & 1u) == 0u ? packed & 0xFu : packed >> 4u);
            result.push_back(decode_adpcm_nibble(nibble));
        }
    }
    return result;
}

void AicaSampleDecoder::reset() noexcept {
    predictor_ = 0;
    step_ = 127;
}

std::int32_t AicaSampleDecoder::predictor() const noexcept { return predictor_; }
std::int32_t AicaSampleDecoder::step() const noexcept { return step_; }

std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory) {
    auto registers = std::make_shared<AicaRegisterFile>();
    auto device = std::make_shared<MmioMemoryDevice>(
        aica_register_size,
        [registers](const std::uint32_t offset, const MemoryAccessWidth width) {
            return registers->read(offset, width);
        },
        [registers](const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            registers->write(offset, value, width);
        }
    );
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + aica_register_physical_base;
        memory.map_region("dreamcast-aica-registers-" + std::to_string(base), base, device);
    }
    return registers;
}

} // namespace katana::runtime
