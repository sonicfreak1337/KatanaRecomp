#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t aica_register_physical_base = 0x00700000u;
inline constexpr std::size_t aica_register_size = 0x00008000u;
inline constexpr std::size_t aica_channel_count = 64u;
inline constexpr std::size_t aica_channel_register_stride = 0x80u;
inline constexpr std::uint32_t aica_common_register_base = 0x2800u;

class AicaRegisterFile final {
public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width) const;
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t write_count() const noexcept;
private:
    [[nodiscard]] static std::size_t width_bytes(MemoryAccessWidth width) noexcept;
    void check(std::uint32_t offset, MemoryAccessWidth width) const;
    std::array<std::uint8_t, aica_register_size> registers_{};
    std::uint64_t writes_ = 0u;
};

enum class AicaSampleFormat : std::uint8_t {
    Pcm16,
    Pcm8,
    Adpcm4
};

class AicaSampleDecoder final {
public:
    explicit AicaSampleDecoder(AicaSampleFormat format) noexcept;
    [[nodiscard]] std::vector<std::int16_t> decode(
        std::span<const std::uint8_t> source,
        std::size_t sample_count
    );
    void reset() noexcept;
    [[nodiscard]] std::int32_t predictor() const noexcept;
    [[nodiscard]] std::int32_t step() const noexcept;
private:
    [[nodiscard]] std::int16_t decode_adpcm_nibble(std::uint8_t nibble) noexcept;
    AicaSampleFormat format_ = AicaSampleFormat::Pcm16;
    std::int32_t predictor_ = 0;
    std::int32_t step_ = 127;
};

inline constexpr std::uint32_t aica_unity_gain = 32768u;
inline constexpr std::int32_t aica_pan_left = -32768;
inline constexpr std::int32_t aica_pan_center = 0;
inline constexpr std::int32_t aica_pan_right = 32768;

struct AicaVoice {
    std::span<const std::int16_t> samples;
    std::uint32_t gain = aica_unity_gain;
    std::int32_t pan = aica_pan_center;
};

class AicaMixer final {
public:
    [[nodiscard]] std::vector<std::int16_t> mix(
        std::span<const AicaVoice> voices,
        std::size_t frame_count
    ) const;
};

class AicaAudioBackend {
public:
    virtual ~AicaAudioBackend() = default;
    virtual void submit(
        std::span<const std::int16_t> interleaved_stereo,
        std::uint32_t sample_rate
    ) = 0;
};

class RecordingAicaAudioBackend final : public AicaAudioBackend {
public:
    void submit(std::span<const std::int16_t> interleaved_stereo, std::uint32_t sample_rate) override;
    [[nodiscard]] std::uint64_t submitted_buffers() const noexcept;
    [[nodiscard]] std::uint64_t submitted_frames() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] const std::vector<std::int16_t>& last_buffer() const noexcept;
private:
    std::uint64_t submitted_buffers_ = 0u;
    std::uint64_t submitted_frames_ = 0u;
    std::uint32_t sample_rate_ = 0u;
    std::vector<std::int16_t> last_buffer_;
};

[[nodiscard]] std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory);

} // namespace katana::runtime
