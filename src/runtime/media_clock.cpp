#include "katana/runtime/media_clock.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::runtime {

namespace {

std::uint64_t checked_multiply(const std::uint64_t left, const std::uint64_t right) {
    if (right != 0u && left > std::numeric_limits<std::uint64_t>::max() / right) {
        throw std::overflow_error("Medienuhr-Kadenz ist uebergelaufen.");
    }
    return left * right;
}

std::uint64_t checked_add(const std::uint64_t left, const std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error("Medienuhr-Deadline ist uebergelaufen.");
    }
    return left + right;
}

} // namespace

DreamcastMediaClock::DreamcastMediaClock(EventScheduler& scheduler,
                                         const MediaClockConfig config,
                                         VideoTickCallback video_callback,
                                         AudioTickCallback audio_callback)
    : scheduler_(scheduler), config_(config), video_callback_(std::move(video_callback)),
      audio_callback_(std::move(audio_callback)),
      video_cadence_(make_cadence(config.guest_cycles_per_second, 1u, config.frame_rate)),
      audio_cadence_(make_cadence(config.guest_cycles_per_second,
                                  config.audio_frames_per_buffer,
                                  config.audio_sample_rate)) {
    if (config_.audio_frames_per_buffer == 0u) {
        throw std::invalid_argument("Audio-Puffergroesse muss groesser null sein.");
    }
}

DreamcastMediaClock::~DreamcastMediaClock() {
    stop();
}

DreamcastMediaClock::Cadence
DreamcastMediaClock::make_cadence(const std::uint64_t cycles_per_second,
                                  const std::uint64_t units_per_tick,
                                  const std::uint64_t units_per_second) {
    if (cycles_per_second == 0u || units_per_tick == 0u || units_per_second == 0u) {
        throw std::invalid_argument("Medienuhr-Raten muessen groesser null sein.");
    }
    const auto whole = cycles_per_second / units_per_second;
    const auto fractional = cycles_per_second % units_per_second;
    const auto fractional_product = checked_multiply(fractional, units_per_tick);
    const auto base =
        checked_add(checked_multiply(whole, units_per_tick), fractional_product / units_per_second);
    if (base == 0u) {
        throw std::invalid_argument("Medienuhr-Kadenz liegt unter einem Gastzyklus.");
    }
    return Cadence{
        base,
        fractional_product % units_per_second,
        units_per_second,
        0u,
        0u,
    };
}

std::uint64_t DreamcastMediaClock::advance_deadline(Cadence& cadence) {
    auto interval = cadence.base_interval;
    if (cadence.remainder != 0u) {
        const auto threshold = cadence.denominator - cadence.remainder;
        if (cadence.phase >= threshold) {
            cadence.phase -= threshold;
            interval = checked_add(interval, 1u);
        } else {
            cadence.phase += cadence.remainder;
        }
    }
    cadence.deadline = checked_add(cadence.deadline, interval);
    return cadence.deadline;
}

void DreamcastMediaClock::start() {
    if (running_) {
        return;
    }
    ++generation_;
    running_ = true;
    video_cadence_.phase = 0u;
    video_cadence_.deadline = scheduler_.current_cycle();
    audio_cadence_.phase = 0u;
    audio_cadence_.deadline = scheduler_.current_cycle();
    try {
        schedule_video();
        schedule_audio();
    } catch (...) {
        stop();
        throw;
    }
}

void DreamcastMediaClock::stop() noexcept {
    ++generation_;
    if (video_event_) {
        static_cast<void>(scheduler_.cancel(*video_event_));
    }
    if (audio_event_) {
        static_cast<void>(scheduler_.cancel(*audio_event_));
    }
    video_event_.reset();
    audio_event_.reset();
    running_ = false;
}

void DreamcastMediaClock::reset() noexcept {
    stop();
    video_ticks_ = 0u;
    audio_ticks_ = 0u;
    emitted_audio_frames_ = 0u;
    video_cadence_.phase = 0u;
    audio_cadence_.phase = 0u;
    video_cadence_.deadline = scheduler_.current_cycle();
    audio_cadence_.deadline = scheduler_.current_cycle();
}

bool DreamcastMediaClock::running() const noexcept {
    return running_;
}
std::uint64_t DreamcastMediaClock::video_tick_count() const noexcept {
    return video_ticks_;
}
std::uint64_t DreamcastMediaClock::audio_tick_count() const noexcept {
    return audio_ticks_;
}
std::uint64_t DreamcastMediaClock::emitted_audio_frames() const noexcept {
    return emitted_audio_frames_;
}

std::optional<std::uint64_t> DreamcastMediaClock::next_video_cycle() const noexcept {
    return video_event_ ? std::optional<std::uint64_t>(video_cadence_.deadline) : std::nullopt;
}

std::optional<std::uint64_t> DreamcastMediaClock::next_audio_cycle() const noexcept {
    return audio_event_ ? std::optional<std::uint64_t>(audio_cadence_.deadline) : std::nullopt;
}

void DreamcastMediaClock::schedule_video() {
    const auto deadline = advance_deadline(video_cadence_);
    const auto generation = generation_;
    video_event_ = scheduler_.schedule_at(
        deadline,
        [this, generation](const auto, const auto) { handle_video(generation); },
        SchedulerEventKind::MediaVideo);
}

void DreamcastMediaClock::schedule_audio() {
    const auto deadline = advance_deadline(audio_cadence_);
    const auto generation = generation_;
    audio_event_ = scheduler_.schedule_at(
        deadline,
        [this, generation](const auto, const auto) { handle_audio(generation); },
        SchedulerEventKind::MediaAudio);
}

void DreamcastMediaClock::handle_video(const std::uint64_t generation) {
    if (generation != generation_) {
        return;
    }
    video_event_.reset();
    const VideoTick tick{video_ticks_, scheduler_.current_cycle()};
    ++video_ticks_;
    try {
        if (video_callback_) {
            video_callback_(tick);
        }
        if (generation == generation_ && running_ && !video_event_) {
            schedule_video();
        }
    } catch (...) {
        stop();
        throw;
    }
}

void DreamcastMediaClock::handle_audio(const std::uint64_t generation) {
    if (generation != generation_) {
        return;
    }
    audio_event_.reset();
    const AudioTick tick{
        audio_ticks_,
        scheduler_.current_cycle(),
        emitted_audio_frames_,
        config_.audio_frames_per_buffer,
    };
    ++audio_ticks_;
    emitted_audio_frames_ = checked_add(emitted_audio_frames_, config_.audio_frames_per_buffer);
    try {
        if (audio_callback_) {
            audio_callback_(tick);
        }
        if (generation == generation_ && running_ && !audio_event_) {
            schedule_audio();
        }
    } catch (...) {
        stop();
        throw;
    }
}

} // namespace katana::runtime
