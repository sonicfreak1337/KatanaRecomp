#include "katana/runtime/aica.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {

AicaRtc::AicaRtc(EventScheduler* scheduler,
                 const std::uint64_t guest_clock_hz,
                 const std::uint32_t initial_seconds)
    : scheduler_(scheduler), guest_clock_hz_(guest_clock_hz), initial_seconds_(initial_seconds),
      base_seconds_(initial_seconds) {
    if (guest_clock_hz_ == 0u) throw std::invalid_argument("AICA-RTC braucht einen Gasttakt.");
    if (scheduler_ != nullptr) {
        scheduler_lifetime_ = scheduler_->lifetime_token();
        base_cycle_ = scheduler_->current_cycle();
        reset_observer_ =
            scheduler_->add_reset_observer([this] { handle_scheduler_reset(); });
    }
}

AicaRtc::~AicaRtc() {
    if (scheduler_ != nullptr && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_->remove_reset_observer(reset_observer_));
}

void AicaRtc::check(const std::uint32_t offset, const MemoryAccessWidth width) {
    if (width != MemoryAccessWidth::Byte && width != MemoryAccessWidth::Halfword &&
        width != MemoryAccessWidth::Word)
        throw std::invalid_argument("Ungueltige AICA-RTC-Zugriffsbreite.");
    if (offset != aica_rtc_high_offset && offset != aica_rtc_low_offset &&
        offset != aica_rtc_control_offset)
        throw std::out_of_range("AICA-RTC-Zugriff trifft kein Register.");
}

std::uint32_t AicaRtc::counter() const noexcept {
    if (scheduler_ == nullptr || scheduler_lifetime_.expired()) return base_seconds_;
    const auto current_cycle = scheduler_->current_cycle();
    const auto elapsed_cycles = current_cycle >= base_cycle_ ? current_cycle - base_cycle_ : 0u;
    return static_cast<std::uint32_t>(base_seconds_ + elapsed_cycles / guest_clock_hz_);
}

std::uint32_t AicaRtc::read(const std::uint32_t offset,
                            const MemoryAccessWidth width) const {
    check(offset, width);
    const auto value = counter();
    if (offset == aica_rtc_high_offset) return value >> 16u;
    if (offset == aica_rtc_low_offset) return value & 0xFFFFu;
    return 0u;
}

void AicaRtc::commit_elapsed() noexcept {
    base_seconds_ = counter();
    if (scheduler_ != nullptr && !scheduler_lifetime_.expired())
        base_cycle_ = scheduler_->current_cycle();
}

void AicaRtc::write(const std::uint32_t offset,
                    const std::uint32_t value,
                    const MemoryAccessWidth width) {
    check(offset, width);
    if (offset == aica_rtc_control_offset) {
        const bool enable = (value & 1u) != 0u;
        if (enable && !write_enabled_) {
            commit_elapsed();
            write_latch_ = base_seconds_;
        }
        write_enabled_ = enable;
        return;
    }
    if (!write_enabled_) return;
    if (offset == aica_rtc_high_offset) {
        write_latch_ = (write_latch_ & 0x0000FFFFu) | ((value & 0xFFFFu) << 16u);
        base_seconds_ = write_latch_;
        if (scheduler_ != nullptr && !scheduler_lifetime_.expired())
            base_cycle_ = scheduler_->current_cycle();
        write_enabled_ = false;
    } else {
        write_latch_ = (write_latch_ & 0xFFFF0000u) | (value & 0xFFFFu);
    }
}

void AicaRtc::reset() noexcept {
    base_seconds_ = initial_seconds_;
    base_cycle_ = scheduler_ != nullptr && !scheduler_lifetime_.expired()
                      ? scheduler_->current_cycle()
                      : 0u;
    write_latch_ = initial_seconds_;
    write_enabled_ = false;
}

bool AicaRtc::write_enabled() const noexcept {
    return write_enabled_;
}

void AicaRtc::handle_scheduler_reset() noexcept {
    base_cycle_ = 0u;
    base_seconds_ = initial_seconds_;
    write_latch_ = initial_seconds_;
    write_enabled_ = false;
}

AicaRegisterFile::AicaRegisterFile(std::shared_ptr<AicaExecutionController> execution,
                                   std::shared_ptr<LinearMemoryDevice> ram)
    : execution_(std::move(execution)), ram_(std::move(ram)) {}

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
    if (offset == 0x2C00u && execution_) {
        auto value = static_cast<std::uint32_t>(registers_[0x2C00u] & 0xFEu) |
                     static_cast<std::uint32_t>(execution_->arm7_reset_asserted());
        if (width != MemoryAccessWidth::Byte)
            value |= static_cast<std::uint32_t>(registers_[offset + 1u]) << 8u;
        return value;
    }
    if (execution_ && width == MemoryAccessWidth::Word) {
        if (offset == 0x28B8u) return execution_->interrupts().pending();
    }
    if (execution_ && width != MemoryAccessWidth::Byte && offset >= 0x2890u && offset <= 0x2898u &&
        (offset & 3u) == 0u) {
        const auto timer = static_cast<std::size_t>((offset - 0x2890u) / 4u);
        const auto stored = static_cast<std::uint32_t>(registers_[offset + 1u] & 7u) << 8u;
        return stored | execution_->timer(timer).counter();
    }
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
    if (offset == 0x2C00u) {
        registers_[offset] &= 1u;
        if (width == MemoryAccessWidth::Word) {
            registers_[offset + 2u] = 0u;
            registers_[offset + 3u] = 0u;
        }
        if (execution_) execution_->set_arm7_reset_asserted((registers_[offset] & 1u) != 0u);
    }
    if (offset < aica_channel_count * aica_channel_register_stride &&
        (offset % aica_channel_register_stride) < 4u) {
        const auto local = offset % aica_channel_register_stride;
        if (local == 0u || (local < 2u && width != MemoryAccessWidth::Byte)) {
            const auto control_offset = offset - local;
            const auto control = static_cast<std::uint16_t>(registers_[control_offset]) |
                                 static_cast<std::uint16_t>(registers_[control_offset + 1u] << 8u);
            if ((control & 0x8000u) != 0u) {
                for (std::size_t channel = 0u; channel < channels_.size(); ++channel) {
                    const auto base = channel * aica_channel_register_stride;
                    const auto candidate = static_cast<std::uint16_t>(registers_[base]) |
                                           static_cast<std::uint16_t>(registers_[base + 1u] << 8u);
                    auto& runtime = channels_[channel];
                    const auto enabled = (candidate & 0x4000u) != 0u;
                    if (enabled && !runtime.active) runtime = ChannelRuntime{0u, 0u, 0, 127, true};
                    if (!enabled) runtime.active = false;
                    registers_[base + 1u] &= 0x7Fu;
                }
            }
        }
    }
    if (execution_ && offset >= 0x2890u && offset <= 0x2898u && (offset & 3u) == 0u) {
        const auto timer = static_cast<std::size_t>((offset - 0x2890u) / 4u);
        execution_->timer(timer).configure(
            static_cast<std::uint8_t>(value), static_cast<std::uint8_t>((value >> 8u) & 7u), true);
    } else if (execution_ && offset == 0x28B4u) {
        execution_->interrupts().set_enabled(value);
    } else if (execution_ && offset == 0x28BCu) {
        execution_->interrupts().acknowledge(value);
    }
    ++writes_;
}

void AicaRegisterFile::reset() noexcept {
    registers_.fill(0u);
    channels_.fill({});
    writes_ = 0u;
    rendered_buffers_ = 0u;
    rendered_frames_ = 0u;
    if (execution_) execution_->set_arm7_reset_asserted(false);
}

std::vector<std::int16_t> AicaRegisterFile::render_audio(const std::size_t frame_count,
                                                         const std::uint32_t sample_rate) {
    if (!ram_) throw std::runtime_error("AICA-Audiopfad besitzt kein gemeinsames Sound-RAM.");
    if (sample_rate == 0u || frame_count > std::numeric_limits<std::size_t>::max() / 2u)
        throw std::invalid_argument("AICA-Audioausgabe besitzt eine ungueltige Geometrie.");
    std::vector<std::int16_t> output(frame_count * 2u, 0);
    std::vector<std::int64_t> accumulation(frame_count * 2u, 0);
    const auto read16 = [this](const std::size_t offset) {
        return static_cast<std::uint16_t>(registers_[offset]) |
               static_cast<std::uint16_t>(registers_[offset + 1u] << 8u);
    };
    const auto master = static_cast<double>(registers_[aica_common_register_base] & 0x0Fu) / 15.0;
    for (std::size_t channel = 0u; channel < channels_.size(); ++channel) {
        auto& runtime = channels_[channel];
        if (!runtime.active) continue;
        const auto base = channel * aica_channel_register_stride;
        const auto control = read16(base);
        const auto format = static_cast<std::uint8_t>((control >> 7u) & 3u);
        const auto sample_base = (static_cast<std::uint32_t>(control & 0x7Fu) << 16u) |
                                 read16(base + 4u);
        const auto loop_start = static_cast<std::uint32_t>(read16(base + 8u));
        const auto loop_end = static_cast<std::uint32_t>(read16(base + 12u));
        if (loop_end == 0u || sample_base >= ram_->size()) {
            runtime.active = false;
            continue;
        }
        const auto pitch = read16(base + 24u);
        auto octave = static_cast<int>((pitch >> 11u) & 0x0Fu);
        if ((octave & 8) != 0) octave -= 16;
        const auto frequency = 44'100.0 * std::ldexp(1.0, octave) *
                               (1.0 + static_cast<double>(pitch & 0x03FFu) / 1024.0);
        const auto phase_step = static_cast<std::uint64_t>(
            std::max(0.0, frequency / sample_rate) * 4294967296.0);
        const auto total_level = registers_[base + 41u];
        const auto direct_level = registers_[base + 37u] & 0x0Fu;
        const auto gain = master * (static_cast<double>(direct_level) / 15.0) *
                          std::pow(10.0, -0.75 * total_level / 20.0);
        const auto pan = registers_[base + 36u] & 0x1Fu;
        const auto pan_attenuation = std::pow(10.0, -3.0 * (pan & 0x0Fu) / 20.0);
        const auto left_gain = gain * (((pan & 0x10u) != 0u) ? 1.0 : pan_attenuation);
        const auto right_gain = gain * (((pan & 0x10u) != 0u) ? pan_attenuation : 1.0);
        const auto reset_adpcm = [&runtime] {
            runtime.adpcm_position = 0u;
            runtime.adpcm_predictor = 0;
            runtime.adpcm_step = 127;
        };
        const auto decode_adpcm_until = [&](const std::uint32_t target) {
            while (runtime.adpcm_position <= target) {
                const auto byte_offset = static_cast<std::uint64_t>(sample_base) +
                                         runtime.adpcm_position / 2u;
                if (byte_offset >= ram_->size()) throw std::out_of_range("AICA-ADPCM verlaesst Sound-RAM.");
                const auto packed = ram_->read_u8(static_cast<std::uint32_t>(byte_offset));
                const auto nibble = static_cast<std::uint8_t>(
                    (runtime.adpcm_position & 1u) == 0u ? packed & 0x0Fu : packed >> 4u);
                static constexpr std::array<std::int32_t, 8u> scale{230,230,230,230,307,409,512,614};
                const auto magnitude = static_cast<std::int32_t>(nibble & 7u);
                const auto delta = ((magnitude * 2 + 1) * runtime.adpcm_step) >> 3;
                runtime.adpcm_predictor += (nibble & 8u) != 0u ? -delta : delta;
                runtime.adpcm_predictor = std::clamp(runtime.adpcm_predictor, -32768, 32767);
                runtime.adpcm_step = std::clamp(
                    (runtime.adpcm_step * scale[static_cast<std::size_t>(magnitude)]) >> 8,
                    127,
                    24576);
                ++runtime.adpcm_position;
            }
            return static_cast<std::int16_t>(runtime.adpcm_predictor);
        };
        for (std::size_t frame = 0u; frame < frame_count && runtime.active; ++frame) {
            auto position = static_cast<std::uint32_t>(runtime.phase >> 32u);
            if (position >= loop_end) {
                if ((control & 0x0200u) == 0u) {
                    runtime.active = false;
                    break;
                }
                position = std::min(loop_start, loop_end - 1u);
                runtime.phase = static_cast<std::uint64_t>(position) << 32u;
                if (format >= 2u) reset_adpcm();
            }
            std::int16_t sample = 0;
            if (format == 0u) {
                const auto address = static_cast<std::uint64_t>(sample_base) + position * 2u;
                if (address + 2u > ram_->size()) throw std::out_of_range("AICA-PCM16 verlaesst Sound-RAM.");
                sample = std::bit_cast<std::int16_t>(ram_->read_u16(static_cast<std::uint32_t>(address)));
            } else if (format == 1u) {
                const auto address = static_cast<std::uint64_t>(sample_base) + position;
                if (address >= ram_->size()) throw std::out_of_range("AICA-PCM8 verlaesst Sound-RAM.");
                sample = static_cast<std::int16_t>(
                    static_cast<std::int16_t>(std::bit_cast<std::int8_t>(ram_->read_u8(static_cast<std::uint32_t>(address)))) * 256);
            } else {
                if (runtime.adpcm_position > position) reset_adpcm();
                sample = decode_adpcm_until(position);
            }
            accumulation[frame * 2u] += static_cast<std::int64_t>(std::lround(sample * left_gain));
            accumulation[frame * 2u + 1u] += static_cast<std::int64_t>(std::lround(sample * right_gain));
            runtime.phase += phase_step;
            if ((control & 0x0200u) == 0u && (runtime.phase >> 32u) >= loop_end)
                runtime.active = false;
        }
    }
    for (std::size_t index = 0u; index < output.size(); ++index)
        output[index] = static_cast<std::int16_t>(
            std::clamp<std::int64_t>(accumulation[index], -32768, 32767));
    ++rendered_buffers_;
    rendered_frames_ += frame_count;
    return output;
}

std::size_t AicaRegisterFile::active_channel_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        channels_.begin(), channels_.end(), [](const auto& channel) { return channel.active; }));
}

std::uint64_t AicaRegisterFile::rendered_buffer_count() const noexcept {
    return rendered_buffers_;
}

std::uint64_t AicaRegisterFile::rendered_frame_count() const noexcept {
    return rendered_frames_;
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

void AicaInterruptState::set_enabled(const std::uint32_t mask) {
    const auto was_asserted = asserted();
    enabled_ = mask;
    if (!was_asserted && asserted() && observer_) observer_();
}

AicaExecutionController::AicaExecutionController(EventScheduler* const scheduler,
                                                 const std::uint64_t guest_clock_hz,
                                                 const std::uint64_t audio_clock_hz)
    : scheduler_(scheduler) {
    if (scheduler_ == nullptr) return;
    if (guest_clock_hz == 0u || audio_clock_hz == 0u ||
        guest_clock_hz > std::numeric_limits<std::uint64_t>::max() / audio_cycles_per_tick)
        throw std::invalid_argument("AICA-Gast- und Audiotakt muessen positiv und darstellbar sein.");
    guest_cycles_per_tick_ =
        std::max<std::uint64_t>(1u, (guest_clock_hz * audio_cycles_per_tick) / audio_clock_hz);
    scheduler_lifetime_ = scheduler_->lifetime_token();
    reset_observer_ = scheduler_->add_reset_observer([this] { handle_scheduler_reset(); });
    schedule_tick();
}

AicaExecutionController::~AicaExecutionController() {
    if (scheduler_ == nullptr || scheduler_lifetime_.expired()) return;
    if (tick_event_) static_cast<void>(scheduler_->cancel(*tick_event_));
    static_cast<void>(scheduler_->remove_reset_observer(reset_observer_));
}

void AicaExecutionController::schedule_tick() {
    if (scheduler_ == nullptr || guest_cycles_per_tick_ == 0u) return;
    tick_event_ = scheduler_->schedule_after(
        guest_cycles_per_tick_, [this](const auto event_id, const auto) { handle_tick(event_id); });
}

void AicaExecutionController::handle_tick(const SchedulerEventId event_id) {
    if (!tick_event_ || *tick_event_ != event_id)
        throw std::logic_error("AICA-Timercompletion besitzt kein aktives Ereignis.");
    tick_event_.reset();
    tick(audio_cycles_per_tick);
    schedule_tick();
}

void AicaExecutionController::handle_scheduler_reset() noexcept {
    tick_event_.reset();
    try {
        schedule_tick();
    } catch (...) {
    }
}
void AicaInterruptState::set_observer(std::function<void()> observer) {
    observer_ = std::move(observer);
}
void AicaInterruptState::request(const std::uint32_t mask) {
    const auto was_asserted = asserted();
    pending_ |= mask;
    if (!was_asserted && asserted() && observer_) observer_();
}
void AicaInterruptState::acknowledge(const std::uint32_t mask) noexcept {
    pending_ &= ~mask;
}
std::uint32_t AicaInterruptState::pending() const noexcept {
    return pending_;
}
std::uint32_t AicaInterruptState::enabled() const noexcept {
    return enabled_;
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

void AicaExecutionController::set_arm7_reset_asserted(const bool asserted) noexcept {
    arm7_reset_asserted_ = asserted;
}

bool AicaExecutionController::arm7_reset_asserted() const noexcept {
    return arm7_reset_asserted_;
}

AicaTimer& AicaExecutionController::timer(const std::size_t index) {
    if (index >= timers_.size()) {
        throw std::out_of_range("Ungueltiger AICA-Timerindex.");
    }
    return timers_[index];
}

const AicaTimer& AicaExecutionController::timer(const std::size_t index) const {
    if (index >= timers_.size()) throw std::out_of_range("Ungueltiger AICA-Timerindex.");
    return timers_[index];
}

AicaInterruptState& AicaExecutionController::interrupts() noexcept {
    return interrupts_;
}

const AicaInterruptState& AicaExecutionController::interrupts() const noexcept {
    return interrupts_;
}

void AicaExecutionController::set_dma_request_observer(std::function<void()> observer) {
    dma_request_observer_ = std::move(observer);
}

void AicaExecutionController::request_dma() {
    if (dma_request_observer_) dma_request_observer_();
}

void AicaExecutionController::tick(const std::uint64_t audio_cycles) {
    for (std::size_t index = 0u; index < timers_.size(); ++index) {
        if (timers_[index].tick(audio_cycles) != 0u) {
            interrupts_.request(timer_interrupt_base << index);
        }
    }
}

std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory) {
    return map_aica_registers(memory, {});
}

std::shared_ptr<AicaRegisterFile>
map_aica_registers(Memory& memory, std::shared_ptr<AicaExecutionController> execution) {
    return map_aica_registers(memory, std::move(execution), {});
}

std::shared_ptr<AicaRegisterFile>
map_aica_registers(Memory& memory,
                   std::shared_ptr<AicaExecutionController> execution,
                   std::shared_ptr<LinearMemoryDevice> ram) {
    auto registers = std::make_shared<AicaRegisterFile>(std::move(execution), std::move(ram));
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

std::shared_ptr<AicaRtc> map_aica_rtc(Memory& memory, EventScheduler* scheduler) {
    auto rtc = std::make_shared<AicaRtc>(scheduler);
    auto device = std::make_shared<MmioMemoryDevice>(
        aica_rtc_register_size,
        [rtc](const std::uint32_t offset, const MemoryAccessWidth width) {
            return rtc->read(offset, width);
        },
        [rtc](const std::uint32_t offset,
              const std::uint32_t value,
              const MemoryAccessWidth width) { rtc->write(offset, value, width); });
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + aica_rtc_physical_base;
        memory.map_region("dreamcast-aica-rtc-" + std::to_string(base), base, device);
    }
    return rtc;
}

} // namespace katana::runtime
