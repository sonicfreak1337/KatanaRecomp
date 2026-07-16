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

std::uint32_t AicaRegisterFile::read(const std::uint32_t offset,
                                     const MemoryAccessWidth width) const {
    check(offset, width);
    std::uint32_t result = 0u;
    for (std::size_t index = 0u; index < width_bytes(width); ++index) {
        result |= static_cast<std::uint32_t>(registers_[offset + index]) << (index * 8u);
    }
    return result;
}

void AicaRegisterFile::write(const std::uint32_t offset,
                             const std::uint32_t value,
                             const MemoryAccessWidth width) {
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

std::uint64_t AicaRegisterFile::write_count() const noexcept {
    return writes_;
}

AicaSampleDecoder::AicaSampleDecoder(const AicaSampleFormat format) noexcept : format_(format) {}

std::int16_t AicaSampleDecoder::decode_adpcm_nibble(const std::uint8_t nibble) noexcept {
    static constexpr std::array<std::int32_t, 8> step_scale = {
        230, 230, 230, 230, 307, 409, 512, 614};
    const auto magnitude = static_cast<std::int32_t>(nibble & 0x7u);
    const auto delta = ((magnitude * 2 + 1) * step_) >> 3;
    predictor_ += (nibble & 0x8u) != 0u ? -delta : delta;
    predictor_ = std::clamp(predictor_, -32768, 32767);
    step_ = (step_ * step_scale[static_cast<std::size_t>(magnitude)]) >> 8;
    step_ = std::clamp(step_, 127, 24576);
    return static_cast<std::int16_t>(predictor_);
}

std::vector<std::int16_t> AicaSampleDecoder::decode(const std::span<const std::uint8_t> source,
                                                    const std::size_t sample_count) {
    if (format_ == AicaSampleFormat::Pcm16 &&
        sample_count > std::numeric_limits<std::size_t>::max() / 2u) {
        throw std::out_of_range("AICA-Samplezahl ist zu gross.");
    }
    const std::size_t required = format_ == AicaSampleFormat::Pcm16 ? sample_count * 2u
                                 : format_ == AicaSampleFormat::Pcm8
                                     ? sample_count
                                     : sample_count / 2u + sample_count % 2u;
    if (source.size() < required) {
        throw std::out_of_range("AICA-Sampledaten sind fuer die angeforderte Samplezahl zu kurz.");
    }
    std::vector<std::int16_t> result;
    result.reserve(sample_count);
    for (std::size_t index = 0u; index < sample_count; ++index) {
        if (format_ == AicaSampleFormat::Pcm16) {
            const auto raw = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(source[index * 2u]) |
                static_cast<std::uint16_t>(source[index * 2u + 1u] << 8u));
            result.push_back(std::bit_cast<std::int16_t>(raw));
        } else if (format_ == AicaSampleFormat::Pcm8) {
            const auto sample = std::bit_cast<std::int8_t>(source[index]);
            result.push_back(static_cast<std::int16_t>(static_cast<std::int16_t>(sample) * 256));
        } else {
            const auto packed = source[index / 2u];
            const auto nibble =
                static_cast<std::uint8_t>((index & 1u) == 0u ? packed & 0xFu : packed >> 4u);
            result.push_back(decode_adpcm_nibble(nibble));
        }
    }
    return result;
}

void AicaSampleDecoder::reset() noexcept {
    predictor_ = 0;
    step_ = 127;
}

std::int32_t AicaSampleDecoder::predictor() const noexcept {
    return predictor_;
}
std::int32_t AicaSampleDecoder::step() const noexcept {
    return step_;
}

std::vector<std::int16_t> AicaMixer::mix(const std::span<const AicaVoice> voices,
                                         const std::size_t frame_count) const {
    if (frame_count > std::numeric_limits<std::size_t>::max() / 2u) {
        throw std::out_of_range("AICA-Mixpuffer ist zu gross.");
    }
    for (const auto& voice : voices) {
        if (voice.gain > aica_unity_gain || voice.pan < aica_pan_left ||
            voice.pan > aica_pan_right) {
            throw std::invalid_argument("AICA-Voice besitzt ungueltige Gain- oder Pan-Werte.");
        }
    }
    std::vector<std::int16_t> output(frame_count * 2u, 0);
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        std::int64_t left = 0;
        std::int64_t right = 0;
        for (const auto& voice : voices) {
            if (frame >= voice.samples.size()) {
                continue;
            }
            const auto left_pan = voice.pan > 0 ? aica_pan_right - voice.pan : aica_pan_right;
            const auto right_pan = voice.pan < 0 ? aica_pan_right + voice.pan : aica_pan_right;
            const auto left_gain =
                static_cast<std::int64_t>(voice.gain) * left_pan / aica_pan_right;
            const auto right_gain =
                static_cast<std::int64_t>(voice.gain) * right_pan / aica_pan_right;
            left += static_cast<std::int64_t>(voice.samples[frame]) * left_gain / aica_unity_gain;
            right += static_cast<std::int64_t>(voice.samples[frame]) * right_gain / aica_unity_gain;
        }
        output[frame * 2u] =
            static_cast<std::int16_t>(std::clamp<std::int64_t>(left, -32768, 32767));
        output[frame * 2u + 1u] =
            static_cast<std::int16_t>(std::clamp<std::int64_t>(right, -32768, 32767));
    }
    return output;
}

void RecordingAicaAudioBackend::submit(const std::span<const std::int16_t> interleaved_stereo,
                                       const std::uint32_t sample_rate) {
    if ((interleaved_stereo.size() & 1u) != 0u || sample_rate == 0u) {
        throw std::invalid_argument(
            "Host-Audio braucht Stereo-Frames und eine gueltige Samplerate.");
    }
    last_buffer_.assign(interleaved_stereo.begin(), interleaved_stereo.end());
    sample_rate_ = sample_rate;
    ++submitted_buffers_;
    submitted_frames_ += interleaved_stereo.size() / 2u;
}

std::uint64_t RecordingAicaAudioBackend::submitted_buffers() const noexcept {
    return submitted_buffers_;
}
std::uint64_t RecordingAicaAudioBackend::submitted_frames() const noexcept {
    return submitted_frames_;
}
std::uint32_t RecordingAicaAudioBackend::sample_rate() const noexcept {
    return sample_rate_;
}
const std::vector<std::int16_t>& RecordingAicaAudioBackend::last_buffer() const noexcept {
    return last_buffer_;
}

void AicaTimer::configure(const std::uint8_t initial_counter,
                          const std::uint8_t divider_scale,
                          const bool enabled) {
    if (divider_scale > 7u) {
        throw std::invalid_argument("AICA-Timerteiler muss zwischen 1 und 128 liegen.");
    }
    counter_ = initial_counter;
    divisor_ = 1u << divider_scale;
    remainder_ = 0u;
    enabled_ = enabled;
}

std::uint64_t AicaTimer::tick(const std::uint64_t audio_cycles) noexcept {
    if (!enabled_) {
        return 0u;
    }
    const auto quotient = audio_cycles / divisor_;
    const auto partial = remainder_ + audio_cycles % divisor_;
    auto increments = quotient + partial / divisor_;
    remainder_ = partial % divisor_;
    auto overflows = increments / 256u;
    increments %= 256u;
    const auto counter = static_cast<std::uint64_t>(counter_) + increments;
    overflows += counter / 256u;
    counter_ = static_cast<std::uint8_t>(counter % 256u);
    return overflows;
}

std::uint8_t AicaTimer::counter() const noexcept {
    return counter_;
}
bool AicaTimer::enabled() const noexcept {
    return enabled_;
}

void AicaInterruptState::set_enabled(const std::uint32_t mask) noexcept {
    enabled_ = mask;
}
void AicaInterruptState::set_observer(std::function<void()> observer) {
    observer_ = std::move(observer);
}
void AicaInterruptState::request(const std::uint32_t mask) {
    pending_ |= mask;
    if ((pending_ & enabled_) != 0u && observer_) observer_();
}
void AicaInterruptState::acknowledge(const std::uint32_t mask) noexcept {
    pending_ &= ~mask;
}
std::uint32_t AicaInterruptState::pending() const noexcept {
    return pending_;
}
bool AicaInterruptState::asserted() const noexcept {
    return (pending_ & enabled_) != 0u;
}

void AicaExecutionController::set_mode(const AicaArm7Mode mode) {
    if (mode == AicaArm7Mode::LowLevelArm7) {
        throw std::runtime_error("AICA-ARM7-LLE ist im v0.29-HLE-Audioprofil nicht implementiert.");
    }
    mode_ = mode;
}

AicaArm7Mode AicaExecutionController::mode() const noexcept {
    return mode_;
}
bool AicaExecutionController::arm7_executes_instructions() const noexcept {
    return false;
}

AicaTimer& AicaExecutionController::timer(const std::size_t index) {
    if (index >= timers_.size()) {
        throw std::out_of_range("Ungueltiger AICA-Timerindex.");
    }
    return timers_[index];
}

AicaInterruptState& AicaExecutionController::interrupts() noexcept {
    return interrupts_;
}

void AicaExecutionController::tick(const std::uint64_t audio_cycles) {
    for (std::size_t index = 0u; index < timers_.size(); ++index) {
        if (timers_[index].tick(audio_cycles) != 0u) {
            interrupts_.request(timer_interrupt_base << index);
        }
    }
}

std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory) {
    auto registers = std::make_shared<AicaRegisterFile>();
    auto device = std::make_shared<MmioMemoryDevice>(
        aica_register_size,
        [registers](const std::uint32_t offset, const MemoryAccessWidth width) {
            return registers->read(offset, width);
        },
        [registers](const std::uint32_t offset,
                    const std::uint32_t value,
                    const MemoryAccessWidth width) { registers->write(offset, value, width); });
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + aica_register_physical_base;
        memory.map_region("dreamcast-aica-registers-" + std::to_string(base), base, device);
    }
    return registers;
}

} // namespace katana::runtime
