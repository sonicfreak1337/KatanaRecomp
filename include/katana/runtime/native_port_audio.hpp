#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_audio_contract_version = 1u;

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
    void poll();
    void pause();
    void resume();
    void stop();
    [[nodiscard]] NativePortAudioSnapshot snapshot() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime
