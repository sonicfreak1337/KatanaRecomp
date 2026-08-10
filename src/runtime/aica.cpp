#include "katana/runtime/aica.hpp"

#include "aica_arm7_core.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "parallel_work.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
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

AicaRtcSnapshot AicaRtc::snapshot() const noexcept {
    return {
        scheduler_ != nullptr && !scheduler_lifetime_.expired()
            ? scheduler_->current_cycle()
            : base_cycle_,
        guest_clock_hz_,
        base_cycle_,
        initial_seconds_,
        base_seconds_,
        counter(),
        write_latch_,
        write_enabled_,
    };
}

void AicaRtc::validate_state_restore(const AicaRtcSnapshot& state) const {
    const auto runtime_cycle =
        scheduler_ != nullptr && !scheduler_lifetime_.expired()
            ? scheduler_->current_cycle()
            : state.base_cycle;
    validate_state_restore(state, runtime_cycle);
}

void AicaRtc::validate_state_restore(
    const AicaRtcSnapshot& state,
    const std::uint64_t expected_scheduler_cycle) const {
    if (state.guest_clock_hz == 0u ||
        state.guest_clock_hz != guest_clock_hz_ ||
        state.initial_seconds != initial_seconds_)
        throw std::invalid_argument(
            "AICA-RTC-Handoff passt nicht zum Runtime-Taktvertrag.");
    if (state.scheduler_cycle != expected_scheduler_cycle ||
        state.base_cycle > state.scheduler_cycle)
        throw std::invalid_argument(
            "AICA-RTC-Handoff passt nicht zur wiederhergestellten Gastzeit.");
    const auto expected_counter = static_cast<std::uint32_t>(
        state.base_seconds +
        (state.scheduler_cycle - state.base_cycle) /
            state.guest_clock_hz);
    if (state.counter != expected_counter)
        throw std::invalid_argument(
            "AICA-RTC-Handoff besitzt einen inkonsistenten Zaehler.");
}

void AicaRtc::restore_state_passive(AicaRtcSnapshot state) {
    validate_state_restore(state);
    commit_validated_state_restore(std::move(state));
}

void AicaRtc::commit_validated_state_restore(
    AicaRtcSnapshot state) noexcept {
    base_cycle_ = state.base_cycle;
    base_seconds_ = state.base_seconds;
    write_latch_ = state.write_latch;
    write_enabled_ = state.write_enabled;
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
    const auto stored_word = [this](const std::uint32_t register_offset) {
        std::uint32_t value = 0u;
        for (std::size_t index = 0u; index < sizeof(value); ++index)
            value |= static_cast<std::uint32_t>(registers_[register_offset + index])
                     << (index * 8u);
        return value;
    };
    const auto logical_byte = [this, &stored_word](const std::uint32_t byte_offset) {
        // The common monitor block is read-driven hardware state. Returning
        // stale backing bytes here can leave retail ARM sound drivers waiting
        // forever for an empty MIDI FIFO or a completed sample channel.
        if (byte_offset == 0x2808u) return std::uint8_t{0u};
        if (byte_offset == 0x2809u) {
            // MIEMP=1 and MOEMP=1; both unimplemented FIFOs are empty.
            return std::uint8_t{0x09u};
        }
        const auto monitored_channel = static_cast<std::size_t>(
            registers_[0x280Du] & 0x3Fu);
        if (byte_offset == 0x2810u || byte_offset == 0x2811u) {
            auto& channel = channels_[monitored_channel];
            const auto envelope = channel.active ? 0u : 0x1FFFu;
            const auto generator_state = channel.active ? 2u : 3u;
            const auto monitor = static_cast<std::uint16_t>(
                envelope | (generator_state << 13u) |
                (channel.looped ? 0x8000u : 0u));
            const auto result = static_cast<std::uint8_t>(
                monitor >> ((byte_offset - 0x2810u) * 8u));
            if (byte_offset == 0x2811u) channel.looped = false;
            return result;
        }
        if (byte_offset == 0x2814u || byte_offset == 0x2815u) {
            const auto current_address = static_cast<std::uint16_t>(
                channels_[monitored_channel].phase >> 32u);
            return static_cast<std::uint8_t>(
                current_address >> ((byte_offset - 0x2814u) * 8u));
        }

        if (!execution_) return registers_[byte_offset];

        constexpr std::uint32_t timer_base = 0x2890u;
        constexpr std::uint32_t timer_stride = 4u;
        constexpr std::uint32_t timer_end =
            timer_base +
            static_cast<std::uint32_t>(AicaExecutionController::timer_count) * timer_stride;
        if (byte_offset >= timer_base && byte_offset < timer_end) {
            const auto timer =
                static_cast<std::size_t>((byte_offset - timer_base) / timer_stride);
            const auto register_offset =
                timer_base + static_cast<std::uint32_t>(timer) * timer_stride;
            const auto value =
                (stored_word(register_offset) & 0xFFFFFF00u) |
                static_cast<std::uint32_t>(execution_->timer(timer).counter());
            return static_cast<std::uint8_t>(
                value >> ((byte_offset - register_offset) * 8u));
        }

        constexpr std::uint32_t interrupt_pending_offset = 0x28B8u;
        if (byte_offset >= interrupt_pending_offset &&
            byte_offset < interrupt_pending_offset + sizeof(std::uint32_t)) {
            return static_cast<std::uint8_t>(
                execution_->interrupts().pending() >>
                ((byte_offset - interrupt_pending_offset) * 8u));
        }

        constexpr std::uint32_t sound_interrupt_pending_offset = 0x28A0u;
        if (byte_offset >= sound_interrupt_pending_offset &&
            byte_offset < sound_interrupt_pending_offset + sizeof(std::uint32_t)) {
            return static_cast<std::uint8_t>(
                execution_->sound_interrupt_pending() >>
                ((byte_offset - sound_interrupt_pending_offset) * 8u));
        }

        constexpr std::uint32_t arm_reset_offset = 0x2C00u;
        if (byte_offset >= arm_reset_offset &&
            byte_offset < arm_reset_offset + sizeof(std::uint32_t)) {
            const auto value =
                (stored_word(arm_reset_offset) & 0xFFFFFFFEu) |
                static_cast<std::uint32_t>(execution_->arm7_reset_asserted());
            return static_cast<std::uint8_t>(
                value >> ((byte_offset - arm_reset_offset) * 8u));
        }
        return registers_[byte_offset];
    };

    std::uint32_t result = 0u;
    for (std::size_t index = 0u; index < width_bytes(width); ++index) {
        result |= static_cast<std::uint32_t>(
                      logical_byte(offset + static_cast<std::uint32_t>(index)))
                  << (index * 8u);
    }
    return result;
}

std::uint32_t AicaRegisterFile::read_arm(const std::uint32_t offset,
                                         const MemoryAccessWidth width) const {
    constexpr std::uint32_t interrupt_level_offset = 0x2D00u;
    constexpr std::uint32_t interrupt_accept_offset = 0x2D04u;
    if (offset == interrupt_level_offset)
        return execution_ ? execution_->arm_interrupt_level() : 0u;
    if (offset == interrupt_accept_offset) return 0u;
    return read(offset, width);
}

void AicaRegisterFile::write_arm(const std::uint32_t offset,
                                 const std::uint32_t value,
                                 const MemoryAccessWidth width) {
    constexpr std::uint32_t interrupt_level_offset = 0x2D00u;
    constexpr std::uint32_t interrupt_accept_offset = 0x2D04u;
    if (offset == interrupt_level_offset) return;
    if (offset == interrupt_accept_offset) {
        if (execution_ && (value & 1u) != 0u) execution_->accept_arm_interrupt();
        return;
    }
    write(offset, value, width);
}

void AicaRegisterFile::write(const std::uint32_t offset,
                             const std::uint32_t value,
                             const MemoryAccessWidth width) {
    check(offset, width);
    const auto written_bytes = width_bytes(width);
    for (std::size_t index = 0u; index < written_bytes; ++index) {
        registers_[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }

    const auto overlaps = [offset, written_bytes](const std::uint32_t register_offset,
                                                  const std::size_t register_bytes) {
        const auto write_begin = static_cast<std::uint64_t>(offset);
        const auto write_end = write_begin + written_bytes;
        const auto register_begin = static_cast<std::uint64_t>(register_offset);
        const auto register_end = register_begin + register_bytes;
        return write_begin < register_end && register_begin < write_end;
    };
    const auto load16 = [this](const std::uint32_t register_offset) {
        return static_cast<std::uint16_t>(registers_[register_offset]) |
               static_cast<std::uint16_t>(
                   static_cast<std::uint16_t>(registers_[register_offset + 1u]) << 8u);
    };
    const auto load32 = [this](const std::uint32_t register_offset) {
        std::uint32_t result = 0u;
        for (std::size_t index = 0u; index < sizeof(result); ++index)
            result |= static_cast<std::uint32_t>(registers_[register_offset + index])
                      << (index * 8u);
        return result;
    };

    if (overlaps(0x2C00u, 4u)) {
        registers_[0x2C00u] &= 1u;
        registers_[0x2C02u] = 0u;
        registers_[0x2C03u] = 0u;
        if (execution_)
            execution_->set_arm7_reset_asserted((registers_[0x2C00u] & 1u) != 0u);
    }

    bool execute_key_on = false;
    for (std::size_t channel = 0u; channel < channels_.size(); ++channel) {
        const auto base = static_cast<std::uint32_t>(channel * aica_channel_register_stride);
        if (overlaps(base, 2u) && (load16(base) & 0x8000u) != 0u) {
            execute_key_on = true;
            break;
        }
    }
    if (execute_key_on) {
        for (std::size_t channel = 0u; channel < channels_.size(); ++channel) {
            const auto base = static_cast<std::uint32_t>(
                channel * aica_channel_register_stride);
            const auto candidate = load16(base);
            auto& runtime = channels_[channel];
            const auto enabled = (candidate & 0x4000u) != 0u;
            if (enabled && !runtime.active) runtime = ChannelRuntime{0u, 0u, 0, 127, true};
            if (!enabled) runtime.active = false;
            registers_[base + 1u] &= 0x7Fu;
        }
    }

    if (execution_) {
        for (std::size_t timer = 0u; timer < AicaExecutionController::timer_count; ++timer) {
            const auto timer_offset = static_cast<std::uint32_t>(0x2890u + timer * 4u);
            if (!overlaps(timer_offset, 2u)) continue;
            const auto timer_value = load16(timer_offset);
            execution_->timer(timer).configure(
                static_cast<std::uint8_t>(timer_value),
                static_cast<std::uint8_t>((timer_value >> 8u) & 7u),
                true);
        }
        if (overlaps(0x289Cu, 4u))
            execution_->set_sound_interrupt_enabled(load32(0x289Cu));
        if (overlaps(0x28A0u, 4u) && (load32(0x28A0u) & (1u << 5u)) != 0u)
            execution_->request_sound_interrupt(1u << 5u);
        if (overlaps(0x28A4u, 4u))
            execution_->acknowledge_sound_interrupt(load32(0x28A4u));
        if (overlaps(0x28A8u, 12u))
            execution_->set_sound_interrupt_levels({
                registers_[0x28A8u], registers_[0x28ACu], registers_[0x28B0u]});
        if (overlaps(0x28B4u, 4u))
            execution_->interrupts().set_enabled(load32(0x28B4u) & 0x7FFu);
        if (overlaps(0x28B8u, 4u) && (load32(0x28B8u) & (1u << 5u)) != 0u)
            execution_->interrupts().request(1u << 5u);
        if (overlaps(0x28BCu, 4u))
            execution_->interrupts().acknowledge(load32(0x28BCu));
    }
    ++writes_;
}

void AicaRegisterFile::reset() noexcept {
    registers_.fill(0u);
    channels_.fill({});
    writes_ = 0u;
    rendered_buffers_ = 0u;
    rendered_frames_ = 0u;
    voice_errors_ = 0u;
    first_voice_error_.reset();
    last_render_jobs_ = 1u;
    parallel_rendered_buffers_ = 0u;
    if (execution_) execution_->reset();
}

void AicaRegisterFile::record_voice_error(const AicaVoiceError error,
                                          const AicaSampleFormat format,
                                          const std::size_t channel,
                                          const std::uint64_t sample_address,
                                          const std::uint64_t rendered_frame) noexcept {
    ++voice_errors_;
    if (!first_voice_error_) {
        first_voice_error_ = AicaVoiceFirstError{
            error,
            format,
            static_cast<std::uint32_t>(channel),
            sample_address,
            rendered_frame,
        };
    }
}

std::vector<std::int16_t> AicaRegisterFile::render_audio(const std::size_t frame_count,
                                                         const std::uint32_t sample_rate) {
    if (!ram_) throw std::runtime_error("AICA-Audiopfad besitzt kein gemeinsames Sound-RAM.");
    if (sample_rate == 0u || frame_count > std::numeric_limits<std::size_t>::max() / 2u)
        throw std::invalid_argument("AICA-Audioausgabe besitzt eine ungueltige Geometrie.");
    std::vector<std::int16_t> output(frame_count * 2u, 0);
    std::vector<std::size_t> active_channels;
    active_channels.reserve(channels_.size());
    for (std::size_t channel = 0u; channel < channels_.size(); ++channel) {
        if (channels_[channel].active) active_channels.push_back(channel);
    }
    if (active_channels.empty()) {
        last_render_jobs_ = 1u;
        ++rendered_buffers_;
        rendered_frames_ += frame_count;
        return output;
    }

    struct ChannelResult {
        ChannelRuntime runtime;
        std::optional<AicaVoiceFirstError> error;
    };
    std::array<ChannelResult, aica_channel_count> channel_results{};
    for (const auto channel : active_channels)
        channel_results[channel].runtime = channels_[channel];

    const auto requested_jobs =
        std::min(active_channels.size(), detail::runtime_parallel_job_capacity());
    std::vector<std::vector<std::int64_t>> job_accumulations(
        requested_jobs, std::vector<std::int64_t>(frame_count * 2u, 0));
    const auto read16 = [this](const std::size_t offset) {
        return static_cast<std::uint16_t>(registers_[offset]) |
               static_cast<std::uint16_t>(registers_[offset + 1u] << 8u);
    };
    const auto ram_size = static_cast<std::uint64_t>(ram_->size());
    const auto master = static_cast<double>(registers_[aica_common_register_base] & 0x0Fu) / 15.0;
    const auto rendered_frame_base = rendered_frames_;
    const auto run_job = [&](const std::size_t job,
                             const std::size_t job_count) {
        auto& accumulation = job_accumulations[job];
        const auto begin = active_channels.size() * job / job_count;
        const auto end = active_channels.size() * (job + 1u) / job_count;
        for (auto active_index = begin; active_index < end; ++active_index) {
            const auto channel = active_channels[active_index];
            auto& result = channel_results[channel];
            auto& runtime = result.runtime;
            const auto base = channel * aica_channel_register_stride;
            const auto control = read16(base);
            const auto format = static_cast<std::uint8_t>((control >> 7u) & 3u);
            const auto sample_format =
                format == 0u ? AicaSampleFormat::Pcm16
                : format == 1u ? AicaSampleFormat::Pcm8
                               : AicaSampleFormat::Adpcm4;
            const auto range_error =
                format == 0u ? AicaVoiceError::Pcm16OutOfRange
                : format == 1u ? AicaVoiceError::Pcm8OutOfRange
                               : AicaVoiceError::AdpcmOutOfRange;
            const auto sample_base =
                (static_cast<std::uint32_t>(control & 0x7Fu) << 16u) |
                read16(base + 4u);
            const auto loop_start =
                static_cast<std::uint32_t>(read16(base + 8u));
            const auto loop_end =
                static_cast<std::uint32_t>(read16(base + 12u));
            if (loop_end == 0u) {
                runtime.active = false;
                continue;
            }
            if (sample_base >= ram_size) {
                result.error = AicaVoiceFirstError{
                    range_error,
                    sample_format,
                    static_cast<std::uint32_t>(channel),
                    sample_base,
                    rendered_frame_base,
                };
                runtime.active = false;
                continue;
            }
            const auto pitch = read16(base + 24u);
            auto octave = static_cast<int>((pitch >> 11u) & 0x0Fu);
            if ((octave & 8) != 0) octave -= 16;
            const auto frequency =
                44'100.0 * std::ldexp(1.0, octave) *
                (1.0 +
                 static_cast<double>(pitch & 0x03FFu) / 1024.0);
            const auto phase_step = static_cast<std::uint64_t>(
                std::max(0.0, frequency / sample_rate) * 4294967296.0);
            const auto total_level = registers_[base + 41u];
            const auto direct_level = registers_[base + 37u] & 0x0Fu;
            const auto gain =
                master * (static_cast<double>(direct_level) / 15.0) *
                std::pow(10.0, -0.75 * total_level / 20.0);
            const auto pan = registers_[base + 36u] & 0x1Fu;
            const auto pan_attenuation =
                std::pow(10.0, -3.0 * (pan & 0x0Fu) / 20.0);
            const auto left_gain =
                gain * (((pan & 0x10u) != 0u) ? 1.0 : pan_attenuation);
            const auto right_gain =
                gain * (((pan & 0x10u) != 0u) ? pan_attenuation : 1.0);
            const auto reset_adpcm = [&runtime] {
                runtime.adpcm_position = 0u;
                runtime.adpcm_predictor = 0;
                runtime.adpcm_step = 127;
            };
            const auto decode_adpcm_until =
                [&](const std::uint32_t target,
                    const std::size_t frame) -> std::optional<std::int16_t> {
                while (runtime.adpcm_position <= target) {
                    const auto byte_offset =
                        static_cast<std::uint64_t>(sample_base) +
                        runtime.adpcm_position / 2u;
                    if (byte_offset >= ram_size) {
                        result.error = AicaVoiceFirstError{
                            AicaVoiceError::AdpcmOutOfRange,
                            AicaSampleFormat::Adpcm4,
                            static_cast<std::uint32_t>(channel),
                            byte_offset,
                            rendered_frame_base + frame,
                        };
                        runtime.active = false;
                        return std::nullopt;
                    }
                    const auto packed =
                        ram_->read_u8(static_cast<std::uint32_t>(byte_offset));
                    const auto nibble = static_cast<std::uint8_t>(
                        (runtime.adpcm_position & 1u) == 0u
                            ? packed & 0x0Fu
                            : packed >> 4u);
                    static constexpr std::array<std::int32_t, 8u> scale{
                        230, 230, 230, 230, 307, 409, 512, 614};
                    const auto magnitude =
                        static_cast<std::int32_t>(nibble & 7u);
                    const auto delta =
                        ((magnitude * 2 + 1) * runtime.adpcm_step) >> 3;
                    runtime.adpcm_predictor +=
                        (nibble & 8u) != 0u ? -delta : delta;
                    runtime.adpcm_predictor =
                        std::clamp(runtime.adpcm_predictor, -32768, 32767);
                    runtime.adpcm_step = std::clamp(
                        (runtime.adpcm_step *
                         scale[static_cast<std::size_t>(magnitude)]) >>
                            8,
                        127,
                        24576);
                    ++runtime.adpcm_position;
                }
                return static_cast<std::int16_t>(
                    runtime.adpcm_predictor);
            };
            for (std::size_t frame = 0u;
                 frame < frame_count && runtime.active;
                 ++frame) {
                auto position =
                    static_cast<std::uint32_t>(runtime.phase >> 32u);
                if (position >= loop_end) {
                    if ((control & 0x0200u) == 0u) {
                        runtime.active = false;
                        break;
                    }
                    position = std::min(loop_start, loop_end - 1u);
                    runtime.phase =
                        static_cast<std::uint64_t>(position) << 32u;
                    runtime.looped = true;
                    if (format >= 2u) reset_adpcm();
                }
                std::int16_t sample = 0;
                if (format == 0u) {
                    const auto address =
                        static_cast<std::uint64_t>(sample_base) +
                        position * 2u;
                    if (address > ram_size || ram_size - address < 2u) {
                        result.error = AicaVoiceFirstError{
                            AicaVoiceError::Pcm16OutOfRange,
                            AicaSampleFormat::Pcm16,
                            static_cast<std::uint32_t>(channel),
                            address,
                            rendered_frame_base + frame,
                        };
                        runtime.active = false;
                        break;
                    }
                    sample = std::bit_cast<std::int16_t>(
                        ram_->read_u16(
                            static_cast<std::uint32_t>(address)));
                } else if (format == 1u) {
                    const auto address =
                        static_cast<std::uint64_t>(sample_base) + position;
                    if (address >= ram_size) {
                        result.error = AicaVoiceFirstError{
                            AicaVoiceError::Pcm8OutOfRange,
                            AicaSampleFormat::Pcm8,
                            static_cast<std::uint32_t>(channel),
                            address,
                            rendered_frame_base + frame,
                        };
                        runtime.active = false;
                        break;
                    }
                    sample = static_cast<std::int16_t>(
                        static_cast<std::int16_t>(
                            std::bit_cast<std::int8_t>(
                                ram_->read_u8(
                                    static_cast<std::uint32_t>(
                                        address)))) *
                        256);
                } else {
                    if (runtime.adpcm_position > position) reset_adpcm();
                    const auto decoded =
                        decode_adpcm_until(position, frame);
                    if (!decoded) break;
                    sample = *decoded;
                }
                accumulation[frame * 2u] +=
                    static_cast<std::int64_t>(
                        std::lround(sample * left_gain));
                accumulation[frame * 2u + 1u] +=
                    static_cast<std::int64_t>(
                        std::lround(sample * right_gain));
                runtime.phase += phase_step;
                if ((control & 0x0200u) == 0u &&
                    (runtime.phase >> 32u) >= loop_end)
                    runtime.active = false;
            }
        }
    };
    const auto render_jobs =
        detail::run_runtime_parallel_work(active_channels.size(), run_job);

    std::vector<std::int64_t> accumulation(frame_count * 2u, 0);
    for (std::size_t job = 0u; job < render_jobs; ++job) {
        for (std::size_t sample = 0u; sample < accumulation.size(); ++sample)
            accumulation[sample] += job_accumulations[job][sample];
    }
    for (const auto channel : active_channels) {
        channels_[channel] = channel_results[channel].runtime;
        if (const auto& error = channel_results[channel].error) {
            record_voice_error(error->error,
                               error->format,
                               error->channel,
                               error->sample_address,
                               error->rendered_frame);
        }
    }
    for (std::size_t index = 0u; index < output.size(); ++index)
        output[index] = static_cast<std::int16_t>(
            std::clamp<std::int64_t>(accumulation[index], -32768, 32767));
    last_render_jobs_ = render_jobs;
    if (render_jobs > 1u) ++parallel_rendered_buffers_;
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

std::uint64_t AicaRegisterFile::voice_error_count() const noexcept {
    return voice_errors_;
}

std::optional<AicaVoiceFirstError> AicaRegisterFile::first_voice_error() const noexcept {
    return first_voice_error_;
}

std::size_t AicaRegisterFile::render_job_capacity() const noexcept {
    return detail::runtime_parallel_job_capacity();
}

std::size_t AicaRegisterFile::last_render_job_count() const noexcept {
    return last_render_jobs_;
}

std::uint64_t AicaRegisterFile::parallel_rendered_buffer_count() const noexcept {
    return parallel_rendered_buffers_;
}

#if defined(_MSC_VER)
#pragma warning(push)
// The public snapshot is intentionally a complete 34 KiB value object. Keeping
// it on the caller-provided return storage preserves the noexcept contract and
// avoids diagnostic-only heap allocation.
#pragma warning(disable : 6262)
#endif
AicaRegisterSnapshot AicaRegisterFile::snapshot() const noexcept {
    AicaRegisterSnapshot result;
    result.registers = registers_;
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        const auto& source = channels_[index];
        result.channels[index] = {source.phase,
                                  source.adpcm_position,
                                  source.adpcm_predictor,
                                  source.adpcm_step,
                                  source.active,
                                  source.looped};
    }
    result.writes = writes_;
    result.rendered_buffers = rendered_buffers_;
    result.rendered_frames = rendered_frames_;
    result.voice_errors = voice_errors_;
    result.first_voice_error = first_voice_error_;
    return result;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void AicaRegisterFile::validate_state_restore(
    const AicaRegisterSnapshot& state) const {
    const auto valid_voice_error = [](const AicaVoiceError error) noexcept {
        switch (error) {
        case AicaVoiceError::Pcm16OutOfRange:
        case AicaVoiceError::Pcm8OutOfRange:
        case AicaVoiceError::AdpcmOutOfRange:
            return true;
        }
        return false;
    };
    const auto valid_sample_format = [](const AicaSampleFormat format) noexcept {
        switch (format) {
        case AicaSampleFormat::Pcm16:
        case AicaSampleFormat::Pcm8:
        case AicaSampleFormat::Adpcm4:
            return true;
        }
        return false;
    };
    for (const auto& channel : state.channels) {
        if (channel.adpcm_predictor < -32768 ||
            channel.adpcm_predictor > 32767 ||
            channel.adpcm_step < 127 || channel.adpcm_step > 24576)
            throw std::invalid_argument(
                "AICA-Register-Handoff besitzt ungueltigen Voicezustand.");
    }
    if (state.first_voice_error) {
        if (state.voice_errors == 0u ||
            !valid_voice_error(state.first_voice_error->error) ||
            !valid_sample_format(state.first_voice_error->format) ||
            state.first_voice_error->channel >= aica_channel_count)
            throw std::invalid_argument(
                "AICA-Register-Handoff besitzt ungueltige Voicefehlerdaten.");
    } else if (state.voice_errors != 0u) {
        throw std::invalid_argument(
            "AICA-Register-Handoff hat Voicefehler ohne ersten Fehler.");
    }
}

void AicaRegisterFile::restore_state_passive(AicaRegisterSnapshot state) {
    validate_state_restore(state);
    commit_validated_state_restore(std::move(state));
}

void AicaRegisterFile::commit_validated_state_restore(
    AicaRegisterSnapshot state) noexcept {
    registers_ = std::move(state.registers);
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        const auto& source = state.channels[index];
        channels_[index] = {
            source.phase,
            source.adpcm_position,
            source.adpcm_predictor,
            source.adpcm_step,
            source.active,
            source.looped,
        };
    }
    writes_ = state.writes;
    rendered_buffers_ = state.rendered_buffers;
    rendered_frames_ = state.rendered_frames;
    voice_errors_ = state.voice_errors;
    first_voice_error_ = std::move(state.first_voice_error);
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

void AicaTimer::reset() noexcept {
    remainder_ = 0u;
    divisor_ = 1u;
    counter_ = 0u;
    enabled_ = false;
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

AicaTimer::Snapshot AicaTimer::snapshot() const noexcept {
    return {remainder_, divisor_, counter_, enabled_};
}

void AicaTimer::validate_state_restore(const Snapshot& state) const {
    if (state.divisor == 0u || state.divisor > 128u ||
        !std::has_single_bit(state.divisor) ||
        state.remainder >= state.divisor)
        throw std::invalid_argument(
            "AICA-Timer-Handoff besitzt einen ungueltigen Teilerzustand.");
}

void AicaTimer::restore_state_passive(Snapshot state) {
    validate_state_restore(state);
    commit_validated_state_restore(std::move(state));
}

void AicaTimer::commit_validated_state_restore(
    Snapshot state) noexcept {
    remainder_ = state.remainder;
    divisor_ = state.divisor;
    counter_ = state.counter;
    enabled_ = state.enabled;
}

void AicaInterruptState::set_enabled(const std::uint32_t mask) {
    const auto was_asserted = asserted();
    enabled_ = mask;
    if (!was_asserted && asserted() && observer_) observer_();
}

void AicaInterruptState::reset() noexcept {
    enabled_ = 0u;
    pending_ = 0u;
}

AicaExecutionController::AicaExecutionController(EventScheduler* const scheduler,
                                                 const std::uint64_t guest_clock_hz,
                                                 const std::uint64_t audio_clock_hz)
    : arm7_(std::make_unique<AicaArm7Core>()), scheduler_(scheduler) {
    if (scheduler_ == nullptr) return;
    if (guest_clock_hz == 0u || audio_clock_hz == 0u ||
        guest_clock_hz > std::numeric_limits<std::uint64_t>::max() / audio_cycles_per_tick)
        throw std::invalid_argument("AICA-Gast- und Audiotakt muessen positiv und darstellbar sein.");
    guest_cycles_per_tick_ =
        std::max<std::uint64_t>(1u, (guest_clock_hz * audio_cycles_per_tick) / audio_clock_hz);
    scheduler_lifetime_ = scheduler_->lifetime_token();
    reset_observer_ = scheduler_->add_reset_observer([this] { handle_scheduler_reset(); });
    try {
        schedule_tick();
    } catch (...) {
        static_cast<void>(scheduler_->remove_reset_observer(reset_observer_));
        throw;
    }
}

AicaExecutionController::~AicaExecutionController() {
    if (scheduler_ == nullptr || scheduler_lifetime_.expired()) return;
    if (tick_event_) static_cast<void>(scheduler_->cancel(*tick_event_));
    static_cast<void>(scheduler_->remove_reset_observer(reset_observer_));
}

void AicaExecutionController::schedule_tick() {
    if (scheduler_ == nullptr || scheduler_lifetime_.expired() ||
        guest_cycles_per_tick_ == 0u)
        return;
    if (tick_event_)
        throw std::logic_error("AICA-Tick ist bereits geplant.");
    tick_event_ = scheduler_->schedule_after(
        guest_cycles_per_tick_,
        [this](const auto event_id, const auto) { handle_tick(event_id); },
        SchedulerEventKind::AicaTick);
}

void AicaExecutionController::handle_tick(const SchedulerEventId event_id) {
    if (!tick_event_ || *tick_event_ != event_id)
        throw std::logic_error("AICA-Timercompletion besitzt kein aktives Ereignis.");
    tick_event_.reset();
    tick(audio_cycles_per_tick);
    if (error_ == AicaExecutionError::Arm7ExecutionFailure) return;
    try {
        schedule_tick();
        error_ = AicaExecutionError::None;
    } catch (...) {
        tick_event_.reset();
        error_ = AicaExecutionError::TickScheduleFailure;
    }
}

void AicaExecutionController::handle_scheduler_reset() noexcept {
    reset();
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

AicaInterruptState::Snapshot AicaInterruptState::snapshot() const noexcept {
    return {enabled_, pending_, asserted()};
}

void AicaInterruptState::validate_state_restore(const Snapshot& state) const {
    if (state.asserted != ((state.pending & state.enabled) != 0u))
        throw std::invalid_argument(
            "AICA-Interrupt-Handoff besitzt einen inkonsistenten Pegel.");
}

void AicaInterruptState::restore_state_passive(Snapshot state) {
    validate_state_restore(state);
    commit_validated_state_restore(std::move(state));
}

void AicaInterruptState::commit_validated_state_restore(
    Snapshot state) noexcept {
    enabled_ = state.enabled;
    pending_ = state.pending;
}

void AicaExecutionController::reset() noexcept {
    mode_ = AicaArm7Mode::HighLevelAudio;
    arm7_reset_asserted_ = false;
    for (auto& timer : timers_) timer.reset();
    interrupts_.reset();
    sound_interrupts_.reset();
    sound_interrupt_levels_.fill(0u);
    arm_interrupt_level_ = 0u;
    arm_interrupt_output_ = false;
    arm7_->reset(false);
    error_ = AicaExecutionError::None;

    if (scheduler_ != nullptr && !scheduler_lifetime_.expired() && tick_event_)
        static_cast<void>(scheduler_->cancel(*tick_event_));
    tick_event_.reset();
    tick_event_rehydration_pending_ = false;
    if (scheduler_ == nullptr || scheduler_lifetime_.expired()) return;
    try {
        schedule_tick();
    } catch (...) {
        tick_event_.reset();
        error_ = AicaExecutionError::TickScheduleFailure;
    }
}

void AicaExecutionController::set_mode(const AicaArm7Mode mode) {
    if (mode == AicaArm7Mode::LowLevelArm7 && arm7_reset_asserted_)
        throw std::logic_error("AICA-ARM7 kann waehrend Reset nicht ausgefuehrt werden.");
    if (mode == AicaArm7Mode::LowLevelArm7 && !arm7_->bus_bound())
        throw std::logic_error("AICA-ARM7 braucht vor der Freigabe einen gebundenen Bus.");
    if (mode == AicaArm7Mode::LowLevelArm7 && arm7_->faulted())
        throw std::logic_error("AICA-ARM7 muss nach einem Ausfuehrungsfehler zurueckgesetzt werden.");
    mode_ = mode;
    arm7_->set_enabled(mode == AicaArm7Mode::LowLevelArm7 && !arm7_reset_asserted_);
}

AicaArm7Mode AicaExecutionController::mode() const noexcept {
    return mode_;
}
bool AicaExecutionController::arm7_executes_instructions() const noexcept {
    return mode_ == AicaArm7Mode::LowLevelArm7 && arm7_->enabled();
}

void AicaExecutionController::set_arm7_reset_asserted(const bool asserted) noexcept {
    if (asserted) {
        if (!arm7_reset_asserted_ || arm7_->enabled()) arm7_->reset(false);
        mode_ = AicaArm7Mode::HighLevelAudio;
        if (error_ == AicaExecutionError::Arm7ExecutionFailure) {
            error_ = AicaExecutionError::None;
            if (scheduler_ != nullptr && !scheduler_lifetime_.expired() && !tick_event_) {
                try {
                    schedule_tick();
                } catch (...) {
                    error_ = AicaExecutionError::TickScheduleFailure;
                }
            }
        }
    } else if (!arm7_->enabled()) {
        if (!arm7_->bus_bound()) {
            mode_ = AicaArm7Mode::HighLevelAudio;
            arm7_->mark_faulted();
            error_ = AicaExecutionError::Arm7ExecutionFailure;
            arm7_reset_asserted_ = asserted;
            return;
        }
        arm7_->reset(true);
        arm7_->set_fiq_line(arm_interrupt_output_);
        mode_ = AicaArm7Mode::LowLevelArm7;
    }
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

void AicaExecutionController::bind_arm7_bus(
    const std::shared_ptr<AicaRegisterFile>& registers,
    const std::shared_ptr<LinearMemoryDevice>& ram) {
    arm7_->bind_bus(registers, ram);
}

void AicaExecutionController::set_sound_interrupt_enabled(const std::uint32_t mask) {
    sound_interrupts_.set_enabled(mask & 0x7FFu);
    refresh_arm_interrupt();
}

void AicaExecutionController::request_sound_interrupt(const std::uint32_t mask) {
    sound_interrupts_.request(mask & 0x7FFu);
    refresh_arm_interrupt();
}

void AicaExecutionController::acknowledge_sound_interrupt(const std::uint32_t mask) noexcept {
    sound_interrupts_.acknowledge(mask & 0x7FFu);
    refresh_arm_interrupt();
}

void AicaExecutionController::set_sound_interrupt_levels(
    const std::array<std::uint8_t, 3u> levels) noexcept {
    sound_interrupt_levels_ = levels;
    refresh_arm_interrupt();
}

std::uint32_t AicaExecutionController::sound_interrupt_pending() const noexcept {
    return sound_interrupts_.pending();
}

std::uint8_t AicaExecutionController::arm_interrupt_level() const noexcept {
    return arm_interrupt_level_;
}

void AicaExecutionController::accept_arm_interrupt() noexcept {
    arm_interrupt_output_ = false;
    arm7_->set_fiq_line(false);
    refresh_arm_interrupt();
}

void AicaExecutionController::refresh_arm_interrupt() noexcept {
    if (!arm_interrupt_output_ && sound_interrupts_.asserted()) {
        const auto active = sound_interrupts_.pending() & sound_interrupts_.enabled();
        std::uint32_t source = 0u;
        while (source < 11u && (active & (1u << source)) == 0u) ++source;
        const auto level_bit = std::min<std::uint32_t>(source, 7u);
        const auto mask = static_cast<std::uint8_t>(1u << level_bit);
        arm_interrupt_level_ = static_cast<std::uint8_t>(
            ((sound_interrupt_levels_[0] & mask) != 0u ? 1u : 0u) |
            ((sound_interrupt_levels_[1] & mask) != 0u ? 2u : 0u) |
            ((sound_interrupt_levels_[2] & mask) != 0u ? 4u : 0u));
        arm_interrupt_output_ = true;
    }
    arm7_->set_fiq_line(arm_interrupt_output_);
}

void AicaExecutionController::tick(const std::uint64_t audio_cycles) {
    constexpr std::uint64_t arm_cycles_per_sample = 512u;
    constexpr std::uint32_t sample_done_interrupt = 1u << 10u;
    for (std::uint64_t sample = 0u; sample < audio_cycles; ++sample) {
        if (arm7_executes_instructions()) {
            arm7_->run_cycles(arm_cycles_per_sample);
            if (arm7_->faulted()) {
                error_ = AicaExecutionError::Arm7ExecutionFailure;
                return;
            }
        }
        for (std::size_t index = 0u; index < timers_.size(); ++index) {
            if (timers_[index].tick(1u) != 0u) {
                const auto interrupt = timer_interrupt_base << index;
                interrupts_.request(interrupt);
                sound_interrupts_.request(interrupt);
            }
        }
        interrupts_.request(sample_done_interrupt);
        sound_interrupts_.request(sample_done_interrupt);
        refresh_arm_interrupt();
    }
}

AicaExecutionController::Snapshot AicaExecutionController::snapshot() const noexcept {
    Snapshot result;
    result.mode = mode_;
    result.arm7_reset_asserted = arm7_reset_asserted_;
    for (std::size_t index = 0u; index < timers_.size(); ++index)
        result.timers[index] = timers_[index].snapshot();
    result.interrupts = interrupts_.snapshot();
    result.sound_interrupts = sound_interrupts_.snapshot();
    result.sound_interrupt_levels = sound_interrupt_levels_;
    result.arm_interrupt_level = arm_interrupt_level_;
    result.arm_interrupt_output = arm_interrupt_output_;
    result.arm7 = arm7_->snapshot();
    result.tick_event = tick_event_;
    result.tick_event_rehydration_pending =
        tick_event_rehydration_pending_;
    result.error = error_;
    result.guest_cycles_per_tick = guest_cycles_per_tick_;
    return result;
}

void AicaExecutionController::validate_state_restore(
    const Snapshot& state) const {
    const auto valid_mode = [](const AicaArm7Mode mode) noexcept {
        switch (mode) {
        case AicaArm7Mode::HighLevelAudio:
        case AicaArm7Mode::LowLevelArm7:
            return true;
        }
        return false;
    };
    const auto valid_error = [](const AicaExecutionError error) noexcept {
        switch (error) {
        case AicaExecutionError::None:
        case AicaExecutionError::TickScheduleFailure:
        case AicaExecutionError::Arm7ExecutionFailure:
            return true;
        }
        return false;
    };
    if (!valid_mode(state.mode) || !valid_error(state.error))
        throw std::invalid_argument(
            "AICA-Execution-Handoff besitzt einen nicht unterstuetzten Modus.");
    for (std::size_t index = 0u; index < timers_.size(); ++index)
        timers_[index].validate_state_restore(state.timers[index]);
    interrupts_.validate_state_restore(state.interrupts);
    sound_interrupts_.validate_state_restore(state.sound_interrupts);
    arm7_->validate_state_restore(state.arm7);
    const auto arm_failed =
        state.error == AicaExecutionError::Arm7ExecutionFailure;
    const auto expected_arm_enabled =
        state.mode == AicaArm7Mode::LowLevelArm7 &&
        !state.arm7_reset_asserted && !arm_failed;
    if (state.arm_interrupt_level > 7u ||
        state.arm7.enabled != expected_arm_enabled ||
        state.arm7.faulted != arm_failed ||
        state.arm7_reset_asserted && state.arm7.enabled)
        throw std::invalid_argument(
            "AICA-Execution-Handoff besitzt inkonsistenten ARM7-Zustand.");
    if (state.tick_event && state.tick_event_rehydration_pending)
        throw std::invalid_argument(
            "AICA-Execution-Handoff darf kein gebundenes und ausstehendes "
            "Tickevent zugleich besitzen.");

    const auto scheduler_bound =
        scheduler_ != nullptr && !scheduler_lifetime_.expired();
    if (!scheduler_bound) {
        if (state.guest_cycles_per_tick != 0u || state.tick_event ||
            state.tick_event_rehydration_pending)
            throw std::invalid_argument(
                "AICA-Execution-Handoff erwartet einen nicht gebundenen Scheduler.");
        return;
    }
    if (state.guest_cycles_per_tick == 0u ||
        state.guest_cycles_per_tick != guest_cycles_per_tick_)
        throw std::invalid_argument(
            "AICA-Execution-Handoff passt nicht zum Runtime-Taktvertrag.");
    const auto has_tick =
        state.tick_event.has_value() ||
        state.tick_event_rehydration_pending;
    if ((state.error == AicaExecutionError::None) != has_tick)
        throw std::invalid_argument(
            "AICA-Execution-Handoff besitzt einen inkonsistenten Tickvertrag.");
}

void AicaExecutionController::restore_state_passive(Snapshot state) {
    validate_state_restore(state);
    commit_validated_state_restore(std::move(state));
}

void AicaExecutionController::commit_validated_state_restore(
    Snapshot state) noexcept {
    const auto needs_tick_rehydration =
        state.tick_event.has_value() ||
        state.tick_event_rehydration_pending;
    if (scheduler_ != nullptr && !scheduler_lifetime_.expired() &&
        tick_event_)
        static_cast<void>(scheduler_->cancel(*tick_event_));
    tick_event_.reset();
    mode_ = state.mode;
    arm7_reset_asserted_ = state.arm7_reset_asserted;
    for (std::size_t index = 0u; index < timers_.size(); ++index)
        timers_[index].commit_validated_state_restore(
            std::move(state.timers[index]));
    interrupts_.commit_validated_state_restore(
        std::move(state.interrupts));
    sound_interrupts_.commit_validated_state_restore(
        std::move(state.sound_interrupts));
    sound_interrupt_levels_ = state.sound_interrupt_levels;
    arm_interrupt_level_ = state.arm_interrupt_level;
    arm_interrupt_output_ = state.arm_interrupt_output;
    arm7_->commit_validated_state_restore(std::move(state.arm7));
    arm7_->set_fiq_line(arm_interrupt_output_);
    error_ = state.error;
    tick_event_rehydration_pending_ = needs_tick_rehydration;
}

SchedulerEventId AicaExecutionController::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != dreamcast_aica_tick_event_channel ||
        token != dreamcast_aica_tick_event_token_v1)
        throw std::invalid_argument(
            "AICA-Execution-Handoff besitzt einen unbekannten Eventkanal "
            "oder Token.");
    if (scheduler_ == nullptr || scheduler_lifetime_.expired() ||
        !tick_event_rehydration_pending_ || tick_event_ ||
        error_ != AicaExecutionError::None)
        throw std::logic_error(
            "AICA-Execution-Handoff erwartet kein Tickevent.");
    if (guest_cycle < scheduler_->current_cycle())
        throw std::invalid_argument(
            "AICA-Tickevent darf nicht in der Vergangenheit liegen.");
    const auto event_id = scheduler_->schedule_at(
        guest_cycle,
        make_rehydrated_scheduled_event_callback(channel, token),
        SchedulerEventKind::AicaTick);
    commit_rehydrated_scheduled_event(event_id, channel, token);
    return event_id;
}

SchedulerCallback
AicaExecutionController::make_rehydrated_scheduled_event_callback(
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != dreamcast_aica_tick_event_channel ||
        token != dreamcast_aica_tick_event_token_v1)
        throw std::invalid_argument(
            "AICA-Execution-Handoff besitzt einen unbekannten Eventkanal "
            "oder Token.");
    return [this](const auto event_id, const auto) {
        handle_tick(event_id);
    };
}

void AicaExecutionController::commit_rehydrated_scheduled_event(
    const SchedulerEventId event_id,
    const std::uint32_t channel,
    const std::uint64_t token) noexcept {
    if (channel != dreamcast_aica_tick_event_channel ||
        token != dreamcast_aica_tick_event_token_v1 ||
        scheduler_ == nullptr || scheduler_lifetime_.expired() ||
        !tick_event_rehydration_pending_ || tick_event_ ||
        error_ != AicaExecutionError::None)
        std::terminate();
    tick_event_ = event_id;
    tick_event_rehydration_pending_ = false;
}

bool AicaExecutionController::event_rehydration_pending() const noexcept {
    return tick_event_rehydration_pending_;
}

DreamcastAicaStateSnapshot snapshot_dreamcast_aica_state(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution) {
    DreamcastAicaStateSnapshot state;
    state.registers = registers.snapshot();
    state.rtc = rtc.snapshot();
    state.execution = execution.snapshot();
    validate_dreamcast_aica_state_restore(
        registers, rtc, execution, state);
    state.execution.tick_event_rehydration_pending =
        state.execution.tick_event.has_value() ||
        state.execution.tick_event_rehydration_pending;
    state.execution.tick_event.reset();
    validate_dreamcast_aica_state_restore(
        registers, rtc, execution, state);
    return state;
}

void normalize_dreamcast_aica_observations_for_restore(
    DreamcastAicaStateSnapshot& state,
    const ObservationRestorePolicy policy) {
    switch (policy) {
    case ObservationRestorePolicy::PreserveCapturedDiagnostics:
        return;
    case ObservationRestorePolicy::FreshProductEpoch:
        break;
    default:
        throw std::invalid_argument(
            "AICA-Handoff besitzt eine unbekannte Beobachtungsrichtlinie.");
    }

    state.registers.writes = 0u;
    state.registers.rendered_buffers = 0u;
    state.registers.rendered_frames = 0u;
    state.registers.voice_errors = 0u;
    state.registers.first_voice_error.reset();
}

void validate_dreamcast_aica_state_restore(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution,
    const DreamcastAicaStateSnapshot& state) {
    const auto expected_scheduler_cycle = state.rtc.scheduler_cycle;
    rtc.validate_state_restore(state.rtc);
    validate_dreamcast_aica_state_restore(
        registers,
        rtc,
        execution,
        state,
        expected_scheduler_cycle);
}

void validate_dreamcast_aica_state_restore(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution,
    const DreamcastAicaStateSnapshot& state,
    const std::uint64_t expected_scheduler_cycle) {
    if (state.contract_version != dreamcast_aica_state_contract_version)
        throw std::invalid_argument(
            "AICA-Handoff besitzt eine unbekannte Vertragsversion.");
    registers.validate_state_restore(state.registers);
    rtc.validate_state_restore(state.rtc, expected_scheduler_cycle);
    execution.validate_state_restore(state.execution);
    if ((state.registers.registers[0x2C00u] & 1u) !=
        static_cast<std::uint8_t>(
            state.execution.arm7_reset_asserted))
        throw std::invalid_argument(
            "AICA-Handoff besitzt inkonsistenten ARM7-Resetzustand.");
}

PreparedDreamcastAicaStateRestore
prepare_dreamcast_aica_state_restore(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution,
    DreamcastAicaStateSnapshot state,
    const std::uint64_t expected_scheduler_cycle) {
    validate_dreamcast_aica_state_restore(
        registers,
        rtc,
        execution,
        state,
        expected_scheduler_cycle);
    PreparedDreamcastAicaStateRestore prepared;
    prepared.state_ = std::move(state);
    return prepared;
}

void commit_dreamcast_aica_state_restore(
    AicaRegisterFile& registers,
    AicaRtc& rtc,
    AicaExecutionController& execution,
    PreparedDreamcastAicaStateRestore prepared) noexcept {
    auto& state = prepared.state_;
    execution.commit_validated_state_restore(
        std::move(state.execution));
    registers.commit_validated_state_restore(
        std::move(state.registers));
    rtc.commit_validated_state_restore(std::move(state.rtc));
}

void restore_dreamcast_aica_state_passive(
    AicaRegisterFile& registers,
    AicaRtc& rtc,
    AicaExecutionController& execution,
    DreamcastAicaStateSnapshot state) {
    const auto expected_scheduler_cycle = state.rtc.scheduler_cycle;
    auto prepared = prepare_dreamcast_aica_state_restore(
        registers,
        rtc,
        execution,
        std::move(state),
        expected_scheduler_cycle);
    commit_dreamcast_aica_state_restore(
        registers, rtc, execution, std::move(prepared));
}

namespace {

constexpr std::array<std::uint8_t, 8u> aica_state_magic{
    'K', 'A', 'T', 'A', 'I', 'C', '1', '\n'};
constexpr std::size_t maximum_aica_state_payload_size = 1u << 20u;

class AicaStateWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void i32(const std::int32_t value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }
    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void raw(const std::span<const std::uint8_t> bytes) {
        if (bytes.size() >
            maximum_aica_state_payload_size - bytes_.size())
            throw std::invalid_argument(
                "AICA-Handoff-Payload ueberschreitet das Groessenlimit.");
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        if (bytes_.size() > maximum_aica_state_payload_size)
            throw std::invalid_argument(
                "AICA-Handoff-Payload ueberschreitet das Groessenlimit.");
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class AicaStateReader final {
  public:
    explicit AicaStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {
        if (bytes_.size() > maximum_aica_state_payload_size)
            throw std::invalid_argument(
                "AICA-Handoff-Payload ueberschreitet das Groessenlimit.");
    }
    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[offset_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "AICA-Handoff-Payload besitzt ein ungueltiges Boolean.");
        return value != 0u;
    }
    [[nodiscard]] std::uint32_t u32() {
        require(4u);
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(
                         bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 4u;
        return value;
    }
    [[nodiscard]] std::int32_t i32() {
        return std::bit_cast<std::int32_t>(u32());
    }
    [[nodiscard]] std::uint64_t u64() {
        require(8u);
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(
                         bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 8u;
        return value;
    }
    void raw(const std::span<std::uint8_t> destination) {
        require(destination.size());
        std::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            destination.size(),
            destination.begin());
        offset_ += destination.size();
    }
    [[nodiscard]] bool matches(
        const std::span<const std::uint8_t> expected) {
        require(expected.size());
        const auto equal = std::equal(
            expected.begin(),
            expected.end(),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ += expected.size();
        return equal;
    }
    void expect_end() const {
        if (offset_ != bytes_.size())
            throw std::invalid_argument(
                "AICA-Handoff-Payload besitzt nachlaufende Bytes.");
    }

  private:
    void require(const std::size_t size) const {
        if (offset_ > bytes_.size() ||
            size > bytes_.size() - offset_)
            throw std::invalid_argument(
                "AICA-Handoff-Payload ist abgeschnitten.");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
};

template <typename Enum>
void write_aica_enum(AicaStateWriter& writer, const Enum value) {
    static_assert(std::is_enum_v<Enum>);
    writer.u32(static_cast<std::uint32_t>(value));
}

template <typename Enum>
[[nodiscard]] Enum read_aica_enum(AicaStateReader& reader) {
    static_assert(std::is_enum_v<Enum>);
    return static_cast<Enum>(reader.u32());
}

void validate_aica_payload_shape(
    const DreamcastAicaStateSnapshot& state) {
    if (state.contract_version != dreamcast_aica_state_contract_version)
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt einen inkompatiblen Vertrag.");
    const auto valid_arm_mode = [](const std::uint32_t mode) noexcept {
        switch (mode) {
        case 0x10u:
        case 0x11u:
        case 0x12u:
        case 0x13u:
        case 0x17u:
        case 0x1Bu:
        case 0x1Fu:
            return true;
        }
        return false;
    };
    for (const auto& channel : state.registers.channels) {
        if (channel.adpcm_predictor < -32768 ||
            channel.adpcm_predictor > 32767 ||
            channel.adpcm_step < 127 ||
            channel.adpcm_step > 24576)
            throw std::invalid_argument(
                "AICA-Handoff-Payload besitzt ungueltigen Voicezustand.");
    }
    if ((state.registers.voice_errors == 0u) !=
        !state.registers.first_voice_error)
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt inkonsistente Voicefehler.");
    if (state.registers.first_voice_error) {
        const auto& error = *state.registers.first_voice_error;
        if (static_cast<std::uint32_t>(error.error) >
                static_cast<std::uint32_t>(
                    AicaVoiceError::AdpcmOutOfRange) ||
            static_cast<std::uint32_t>(error.format) >
                static_cast<std::uint32_t>(
                    AicaSampleFormat::Adpcm4) ||
            error.channel >= aica_channel_count)
            throw std::invalid_argument(
                "AICA-Handoff-Payload besitzt ungueltige Voicefehlerdaten.");
    }
    if (state.rtc.guest_clock_hz == 0u ||
        state.rtc.base_cycle > state.rtc.scheduler_cycle ||
        state.rtc.counter != static_cast<std::uint32_t>(
            state.rtc.base_seconds +
            (state.rtc.scheduler_cycle - state.rtc.base_cycle) /
                state.rtc.guest_clock_hz))
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt ungueltigen RTC-Zustand.");
    if ((state.execution.mode != AicaArm7Mode::HighLevelAudio &&
         state.execution.mode != AicaArm7Mode::LowLevelArm7) ||
        (state.execution.error != AicaExecutionError::None &&
         state.execution.error !=
             AicaExecutionError::TickScheduleFailure &&
         state.execution.error !=
             AicaExecutionError::Arm7ExecutionFailure) ||
        state.execution.tick_event)
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt ungueltigen Executionzustand.");
    for (const auto& timer : state.execution.timers) {
        if (timer.divisor == 0u || timer.divisor > 128u ||
            !std::has_single_bit(timer.divisor) ||
            timer.remainder >= timer.divisor)
            throw std::invalid_argument(
                "AICA-Handoff-Payload besitzt ungueltigen Timerzustand.");
    }
    if (state.execution.interrupts.asserted !=
        ((state.execution.interrupts.pending &
          state.execution.interrupts.enabled) != 0u))
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt ungueltigen Interruptpegel.");
    const auto arm_failed =
        state.execution.error == AicaExecutionError::Arm7ExecutionFailure;
    const auto expected_arm_enabled =
        state.execution.mode == AicaArm7Mode::LowLevelArm7 &&
        !state.execution.arm7_reset_asserted && !arm_failed;
    if (state.execution.sound_interrupts.asserted !=
            ((state.execution.sound_interrupts.pending &
              state.execution.sound_interrupts.enabled) != 0u) ||
        state.execution.arm_interrupt_level > 7u ||
        !valid_arm_mode(state.execution.arm7.registers[16u] & 0x1Fu) ||
        state.execution.arm7.phased_operation > 2u ||
        state.execution.arm7.phase > 16u ||
        state.execution.arm7.block.cycle > 16u ||
        state.execution.arm7.block.register_count > 16u ||
        state.execution.arm7.instruction_cycles > 64u ||
        state.execution.arm7.cycle_debt > 64u ||
        state.execution.arm7.enabled != expected_arm_enabled ||
        state.execution.arm7.faulted != arm_failed)
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt ungueltigen ARM7-Zustand.");
    if (state.execution.guest_cycles_per_tick == 0u) {
        if (state.execution.tick_event_rehydration_pending)
            throw std::invalid_argument(
                "AICA-Handoff-Payload erwartet einen nicht gebundenen Scheduler.");
    } else if ((state.execution.error == AicaExecutionError::None) !=
               state.execution.tick_event_rehydration_pending) {
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt einen inkonsistenten Tickvertrag.");
    }
    if ((state.registers.registers[0x2C00u] & 1u) !=
        static_cast<std::uint8_t>(
            state.execution.arm7_reset_asserted))
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt inkonsistenten ARM7-Resetzustand.");
}

} // namespace

std::vector<std::uint8_t> encode_dreamcast_aica_state(
    const DreamcastAicaStateSnapshot& state) {
    validate_aica_payload_shape(state);
    AicaStateWriter writer;
    writer.raw(aica_state_magic);
    writer.u32(dreamcast_aica_state_contract_version);
    writer.raw(state.registers.registers);
    for (const auto& channel : state.registers.channels) {
        writer.u64(channel.phase);
        writer.u32(channel.adpcm_position);
        writer.i32(channel.adpcm_predictor);
        writer.i32(channel.adpcm_step);
        writer.boolean(channel.active);
        writer.boolean(channel.looped);
    }
    writer.u64(state.registers.writes);
    writer.u64(state.registers.rendered_buffers);
    writer.u64(state.registers.rendered_frames);
    writer.u64(state.registers.voice_errors);
    writer.boolean(state.registers.first_voice_error.has_value());
    if (state.registers.first_voice_error) {
        const auto& error = *state.registers.first_voice_error;
        write_aica_enum(writer, error.error);
        write_aica_enum(writer, error.format);
        writer.u32(error.channel);
        writer.u64(error.sample_address);
        writer.u64(error.rendered_frame);
    }
    writer.u64(state.rtc.scheduler_cycle);
    writer.u64(state.rtc.guest_clock_hz);
    writer.u64(state.rtc.base_cycle);
    writer.u32(state.rtc.initial_seconds);
    writer.u32(state.rtc.base_seconds);
    writer.u32(state.rtc.counter);
    writer.u32(state.rtc.write_latch);
    writer.boolean(state.rtc.write_enabled);
    write_aica_enum(writer, state.execution.mode);
    writer.boolean(state.execution.arm7_reset_asserted);
    for (const auto& timer : state.execution.timers) {
        writer.u64(timer.remainder);
        writer.u32(timer.divisor);
        writer.u8(timer.counter);
        writer.boolean(timer.enabled);
    }
    writer.u32(state.execution.interrupts.enabled);
    writer.u32(state.execution.interrupts.pending);
    writer.boolean(state.execution.interrupts.asserted);
    writer.u32(state.execution.sound_interrupts.enabled);
    writer.u32(state.execution.sound_interrupts.pending);
    writer.boolean(state.execution.sound_interrupts.asserted);
    for (const auto level : state.execution.sound_interrupt_levels) writer.u8(level);
    writer.u8(state.execution.arm_interrupt_level);
    writer.boolean(state.execution.arm_interrupt_output);
    for (const auto value : state.execution.arm7.registers) writer.u32(value);
    for (const auto value : state.execution.arm7.prefetch_opcodes) writer.u32(value);
    writer.u32(state.execution.arm7.prefetch_pc);
    writer.u32(state.execution.arm7.instruction_cycles);
    writer.u32(state.execution.arm7.phased_opcode);
    writer.u32(state.execution.arm7.phased_operation);
    writer.u32(state.execution.arm7.phase);
    writer.u32(state.execution.arm7.block.address);
    writer.u32(state.execution.arm7.block.r15_offset);
    writer.u32(state.execution.arm7.block.last_bank);
    writer.u32(state.execution.arm7.block.base_address);
    writer.u32(state.execution.arm7.block.cycle);
    writer.u32(state.execution.arm7.block.register_count);
    writer.u64(state.execution.arm7.executed_instructions);
    writer.u64(state.execution.arm7.executed_cycles);
    writer.u64(state.execution.arm7.cycle_debt);
    writer.boolean(state.execution.arm7.next_fetch_sequential);
    writer.boolean(state.execution.arm7.waiting_for_interrupt);
    writer.boolean(state.execution.arm7.enabled);
    writer.boolean(state.execution.arm7.faulted);
    writer.boolean(
        state.execution.tick_event_rehydration_pending);
    write_aica_enum(writer, state.execution.error);
    writer.u64(state.execution.guest_cycles_per_tick);
    return std::move(writer).finish();
}

DreamcastAicaStateSnapshot decode_dreamcast_aica_state(
    const std::span<const std::uint8_t> bytes) {
    AicaStateReader reader(bytes);
    if (!reader.matches(aica_state_magic))
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt keine gueltige Signatur.");
    if (reader.u32() != dreamcast_aica_state_contract_version)
        throw std::invalid_argument(
            "AICA-Handoff-Payload besitzt einen inkompatiblen Vertrag.");
    DreamcastAicaStateSnapshot state;
    reader.raw(state.registers.registers);
    for (auto& channel : state.registers.channels) {
        channel.phase = reader.u64();
        channel.adpcm_position = reader.u32();
        channel.adpcm_predictor = reader.i32();
        channel.adpcm_step = reader.i32();
        channel.active = reader.boolean();
        channel.looped = reader.boolean();
    }
    state.registers.writes = reader.u64();
    state.registers.rendered_buffers = reader.u64();
    state.registers.rendered_frames = reader.u64();
    state.registers.voice_errors = reader.u64();
    if (reader.boolean()) {
        state.registers.first_voice_error = AicaVoiceFirstError{
            read_aica_enum<AicaVoiceError>(reader),
            read_aica_enum<AicaSampleFormat>(reader),
            reader.u32(),
            reader.u64(),
            reader.u64(),
        };
    }
    state.rtc.scheduler_cycle = reader.u64();
    state.rtc.guest_clock_hz = reader.u64();
    state.rtc.base_cycle = reader.u64();
    state.rtc.initial_seconds = reader.u32();
    state.rtc.base_seconds = reader.u32();
    state.rtc.counter = reader.u32();
    state.rtc.write_latch = reader.u32();
    state.rtc.write_enabled = reader.boolean();
    state.execution.mode =
        read_aica_enum<AicaArm7Mode>(reader);
    state.execution.arm7_reset_asserted = reader.boolean();
    for (auto& timer : state.execution.timers) {
        timer.remainder = reader.u64();
        timer.divisor = reader.u32();
        timer.counter = reader.u8();
        timer.enabled = reader.boolean();
    }
    state.execution.interrupts.enabled = reader.u32();
    state.execution.interrupts.pending = reader.u32();
    state.execution.interrupts.asserted = reader.boolean();
    state.execution.sound_interrupts.enabled = reader.u32();
    state.execution.sound_interrupts.pending = reader.u32();
    state.execution.sound_interrupts.asserted = reader.boolean();
    for (auto& level : state.execution.sound_interrupt_levels) level = reader.u8();
    state.execution.arm_interrupt_level = reader.u8();
    state.execution.arm_interrupt_output = reader.boolean();
    for (auto& value : state.execution.arm7.registers) value = reader.u32();
    for (auto& value : state.execution.arm7.prefetch_opcodes) value = reader.u32();
    state.execution.arm7.prefetch_pc = reader.u32();
    state.execution.arm7.instruction_cycles = reader.u32();
    state.execution.arm7.phased_opcode = reader.u32();
    state.execution.arm7.phased_operation = reader.u32();
    state.execution.arm7.phase = reader.u32();
    state.execution.arm7.block.address = reader.u32();
    state.execution.arm7.block.r15_offset = reader.u32();
    state.execution.arm7.block.last_bank = reader.u32();
    state.execution.arm7.block.base_address = reader.u32();
    state.execution.arm7.block.cycle = reader.u32();
    state.execution.arm7.block.register_count = reader.u32();
    state.execution.arm7.executed_instructions = reader.u64();
    state.execution.arm7.executed_cycles = reader.u64();
    state.execution.arm7.cycle_debt = reader.u64();
    state.execution.arm7.next_fetch_sequential = reader.boolean();
    state.execution.arm7.waiting_for_interrupt = reader.boolean();
    state.execution.arm7.enabled = reader.boolean();
    state.execution.arm7.faulted = reader.boolean();
    state.execution.tick_event.reset();
    state.execution.tick_event_rehydration_pending =
        reader.boolean();
    state.execution.error =
        read_aica_enum<AicaExecutionError>(reader);
    state.execution.guest_cycles_per_tick = reader.u64();
    reader.expect_end();
    validate_aica_payload_shape(state);
    return state;
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
    auto bound_execution = execution;
    auto bound_ram = ram;
    auto registers = std::make_shared<AicaRegisterFile>(std::move(execution), std::move(ram));
    if (bound_execution && bound_ram)
        bound_execution->bind_arm7_bus(registers, bound_ram);
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
