#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_audio_contract_version = 3u;

struct NativePortAudioFormat final {
    std::uint32_t sample_rate = 44'100u;
    std::uint16_t channels = 2u;
};

struct NativePortAudioConfig final {
    NativePortAudioFormat format;
    // Submission is non-blocking until this many frames are queued.  The
    // caller must pump/poll and retry after the host device consumes data.
    std::uint32_t maximum_queued_frames = 44'100u;
};

enum class NativePortAudioState : std::uint8_t {
    Ready,
    Running,
    Paused,
    Stopped,
    Failed,
};

struct NativePortAudioSnapshot final {
    NativePortAudioState state = NativePortAudioState::Ready;
    std::uint64_t submitted_buffers = 0u;
    std::uint64_t submitted_frames = 0u;
    std::uint64_t completed_frames = 0u;
    std::uint64_t queued_frames = 0u;
    std::uint64_t playback_position_queries = 0u;
    std::uint32_t error_code = 0u;
};

// Native PCM output for port-side decoders and title audio hooks.  This type
// owns only a host audio endpoint; it has no AICA registers, command ring,
// guest firmware, guest timing model or device fallback. Construction, use
// and destruction are confined to one owner thread.
class NativePortAudioStream final {
  public:
    explicit NativePortAudioStream(const NativePortAudioConfig& config = {});
    ~NativePortAudioStream();

    NativePortAudioStream(const NativePortAudioStream&) = delete;
    NativePortAudioStream& operator=(const NativePortAudioStream&) = delete;
    NativePortAudioStream(NativePortAudioStream&&) = delete;
    NativePortAudioStream& operator=(NativePortAudioStream&&) = delete;

    [[nodiscard]] const NativePortAudioFormat& format() const noexcept;
    [[nodiscard]] bool submit_pcm_s16(std::span<const std::int16_t> interleaved_samples);
    // Retires completed host buffers only. This remains cheap when submit()
    // invokes it repeatedly while filling one outer audio/movie pump.
    void poll();
    // Samples the host device cursor exactly where an outer engine/movie pump
    // needs a fresh master clock. Call once per outer pump, never per block.
    void refresh_playback_position();
    void pause();
    void resume();
    void stop();
    [[nodiscard]] NativePortAudioSnapshot snapshot() const noexcept;
    // Monotonic host-device playback position, in interleaved PCM frames from
    // this stream's first submission. Native decoders use the cursor to bound
    // queued audio by what the endpoint has actually consumed rather than by
    // whole-buffer retirement. Movie presentation may use the same value as
    // its master clock. The value never exceeds submitted_frames in snapshot().
    [[nodiscard]] std::uint64_t playback_position_frames() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime
