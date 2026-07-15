#pragma once

#include "katana/runtime/scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace katana::runtime {

struct MediaClockConfig {
    std::uint64_t guest_cycles_per_second = 200'000'000u;
    std::uint32_t frame_rate = 60u;
    std::uint32_t audio_sample_rate = 44'100u;
    std::uint32_t audio_frames_per_buffer = 735u;
};

struct VideoTick {
    std::uint64_t frame_index = 0u;
    std::uint64_t guest_cycle = 0u;
};

struct AudioTick {
    std::uint64_t buffer_index = 0u;
    std::uint64_t guest_cycle = 0u;
    std::uint64_t first_sample_frame = 0u;
    std::uint32_t frame_count = 0u;
};

using VideoTickCallback = std::function<void(const VideoTick&)>;
using AudioTickCallback = std::function<void(const AudioTick&)>;

class DreamcastMediaClock final {
public:
    DreamcastMediaClock(
        EventScheduler& scheduler,
        MediaClockConfig config,
        VideoTickCallback video_callback = {},
        AudioTickCallback audio_callback = {}
    );
    ~DreamcastMediaClock();
    DreamcastMediaClock(const DreamcastMediaClock&) = delete;
    DreamcastMediaClock& operator=(const DreamcastMediaClock&) = delete;

    void start();
    void stop() noexcept;
    void reset() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint64_t video_tick_count() const noexcept;
    [[nodiscard]] std::uint64_t audio_tick_count() const noexcept;
    [[nodiscard]] std::uint64_t emitted_audio_frames() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_video_cycle() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_audio_cycle() const noexcept;

private:
    struct Cadence {
        std::uint64_t base_interval = 0u;
        std::uint64_t remainder = 0u;
        std::uint64_t denominator = 1u;
        std::uint64_t phase = 0u;
        std::uint64_t deadline = 0u;
    };

    static Cadence make_cadence(
        std::uint64_t cycles_per_second,
        std::uint64_t units_per_tick,
        std::uint64_t units_per_second
    );
    static std::uint64_t advance_deadline(Cadence& cadence);
    void schedule_video();
    void schedule_audio();
    void handle_video();
    void handle_audio();

    EventScheduler& scheduler_;
    MediaClockConfig config_;
    VideoTickCallback video_callback_;
    AudioTickCallback audio_callback_;
    Cadence video_cadence_{};
    Cadence audio_cadence_{};
    std::optional<SchedulerEventId> video_event_;
    std::optional<SchedulerEventId> audio_event_;
    std::uint64_t video_ticks_ = 0u;
    std::uint64_t audio_ticks_ = 0u;
    std::uint64_t emitted_audio_frames_ = 0u;
    bool running_ = false;
};

} // namespace katana::runtime
