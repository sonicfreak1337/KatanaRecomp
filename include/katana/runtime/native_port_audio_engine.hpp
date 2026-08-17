#pragma once

#include "katana/runtime/native_port_audio.hpp"
#include "katana/runtime/native_port_codec.hpp"
#include "katana/runtime/native_port_platform.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace katana::runtime {

class NativePortSoundBankEngine;

inline constexpr std::uint32_t native_port_audio_engine_contract_version = 4u;

enum class NativePortAudioEngineFailure : std::uint8_t {
    None,
    InvalidConfig,
    InvalidProvider,
    InvalidHandle,
    InvalidVoiceConfig,
    ContentOpen,
    DecoderOpen,
    DecoderRead,
    InvalidAudioBuffer,
    UnexpectedVideo,
    ResourceLimit,
    HostAudio,
    ThreadViolation,
};

class NativePortAudioEngineError final : public std::runtime_error {
  public:
    NativePortAudioEngineError(NativePortAudioEngineFailure failure,
                               std::uint32_t provider_error_code,
                               std::string_view operation);

    [[nodiscard]] NativePortAudioEngineFailure failure() const noexcept;
    [[nodiscard]] std::uint32_t provider_error_code() const noexcept;

  private:
    NativePortAudioEngineFailure failure_;
    std::uint32_t provider_error_code_;
};

struct NativePortAudioEngineConfig final {
    NativePortAudioFormat output_format{44'100u, 2u};
    std::uint32_t maximum_voices = 64u;
    std::uint32_t maximum_output_queue_frames = 44'100u;
    // Keep enough native PCM queued to survive an ordinary late 60-Hz host
    // frame without giving pause/stop/gain transitions movie-sized latency.
    // The mixer accounts against the device playback cursor rather than
    // whole retained endpoint blocks, so this is a real ~46-ms horizon at
    // 44.1 kHz rather than an imprecise allocation watermark.
    std::uint32_t target_output_queue_frames = 2'048u;
    std::uint32_t mix_block_frames = 256u;
    std::uint32_t maximum_buffered_frames_per_voice = 88'200u;
    std::uint32_t maximum_decoder_reads_per_pump = 4'096u;
};

struct NativePortAudioVoiceConfig final {
    float gain = 1.0f;
    float pan = 0.0f;
    bool loop = false;
    // Positions are expressed in decoded output-rate frames. The first pass
    // starts at start_frame. A looping voice restarts at loop_start_frame;
    // loop_end_frame==0 selects the decoder's exact audio end.
    std::uint64_t start_frame = 0u;
    std::uint64_t loop_start_frame = 0u;
    std::uint64_t loop_end_frame = 0u;
};

// Format-level metadata for one identity-bound CRI ADX stream.  This is a
// native content contract: it describes encoded media and authored loop
// points only; no AICA state, guest sound RAM or CRI command surface is
// exposed to the product runtime.
struct NativePortAdxStreamMetadata final {
    std::uint32_t sample_rate = 0u;
    std::uint32_t channels = 0u;
    std::uint64_t sample_frames = 0u;
    bool loop = false;
    std::uint64_t loop_start_frame = 0u;
    std::uint64_t loop_end_frame = 0u;
};

[[nodiscard]] NativePortAdxStreamMetadata inspect_native_port_adx_content(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding);

// Converts source-rate ADX loop positions into the audio engine's decoded
// output-rate frame domain with overflow-checked integer arithmetic.
[[nodiscard]] NativePortAudioVoiceConfig native_port_adx_voice_config(
    const NativePortAdxStreamMetadata& metadata,
    std::uint32_t output_sample_rate,
    float gain = 1.0f,
    float pan = 0.0f);

struct NativePortAudioVoiceHandle final {
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t generation = 0u;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return slot != 0xFFFFFFFFu && generation != 0u;
    }
};

enum class NativePortAudioVoiceSource : std::uint8_t {
    Codec,
    PcmFeed,
};

enum class NativePortAudioVoiceState : std::uint8_t {
    Ready,
    Playing,
    Paused,
    Completed,
    Stopped,
    Failed,
};

struct NativePortAudioVoiceSnapshot final {
    NativePortAudioVoiceSource source = NativePortAudioVoiceSource::Codec;
    NativePortAudioVoiceState state = NativePortAudioVoiceState::Ready;
    std::uint64_t duration_nanoseconds = 0u;
    std::uint64_t decoded_source_frames = 0u;
    std::uint64_t mixed_output_frames = 0u;
    // Frames from this voice that the monotone host-device cursor has
    // actually crossed. This excludes the endpoint's submitted queue horizon.
    std::uint64_t played_output_frames = 0u;
    std::uint64_t buffered_frames = 0u;
    std::uint64_t loop_count = 0u;
    float gain = 1.0f;
    float pan = 0.0f;
    NativePortAudioEngineFailure failure = NativePortAudioEngineFailure::None;
    std::uint32_t provider_error_code = 0u;
};

struct NativePortAudioEngineSnapshot final {
    std::uint64_t created_voices = 0u;
    std::uint64_t released_voices = 0u;
    std::uint64_t decoder_reads = 0u;
    std::uint64_t decoded_source_frames = 0u;
    std::uint64_t submitted_feed_frames = 0u;
    std::uint64_t mixed_output_frames = 0u;
    std::uint64_t submitted_output_frames = 0u;
    std::uint32_t active_voices = 0u;
    std::uint32_t playing_voices = 0u;
    std::uint32_t failed_voices = 0u;
    bool output_paused = false;
    NativePortAudioSnapshot output;
};

// Native title-audio service. Exact content ranges are identity-verified by
// NativePortPlatformServices, decoded through one statically linked codec
// provider and mixed into one host PCM endpoint. It models voices and host
// audio only: there is no AICA register surface, ARM7 execution, guest sound
// RAM, command ring, interrupt, DMA or device-timing fallback.
//
// Construction, all calls and destruction are owner-thread confined. Voice
// handles are generation checked; stale handles always fail closed.
class NativePortAudioEngine final {
  public:
    NativePortAudioEngine(NativePortPlatformServices& platform,
                          const NativePortCodecProvider& codec_provider,
                          const NativePortAudioEngineConfig& config = {});
    ~NativePortAudioEngine();

    NativePortAudioEngine(const NativePortAudioEngine&) = delete;
    NativePortAudioEngine& operator=(const NativePortAudioEngine&) = delete;
    NativePortAudioEngine(NativePortAudioEngine&&) = delete;
    NativePortAudioEngine& operator=(NativePortAudioEngine&&) = delete;

    [[nodiscard]] NativePortAudioVoiceHandle create_voice(
        const NativePortContentFileBinding& binding,
        const NativePortAudioVoiceConfig& config = {});
    // A bounded stereo/output-rate feed lets native sound-bank and sequencer
    // providers share this exact mixer and host endpoint with codec voices.
    // It is a host PCM source, not guest sound RAM or an AICA command path.
    [[nodiscard]] NativePortAudioVoiceHandle create_pcm_feed(
        const NativePortAudioVoiceConfig& config = {});
    [[nodiscard]] bool submit_pcm_s16(
        NativePortAudioVoiceHandle voice,
        std::span<const std::int16_t> interleaved_stereo_samples);
    void finish_pcm_feed(NativePortAudioVoiceHandle voice);
    void play(NativePortAudioVoiceHandle voice);
    void pause(NativePortAudioVoiceHandle voice);
    void resume(NativePortAudioVoiceHandle voice);
    void stop(NativePortAudioVoiceHandle voice);
    void release(NativePortAudioVoiceHandle voice);
    void set_gain_pan(NativePortAudioVoiceHandle voice, float gain, float pan);
    void set_output_paused(bool paused);
    void stop_all();
    void pump();

    [[nodiscard]] NativePortAudioVoiceSnapshot
    voice_snapshot(NativePortAudioVoiceHandle voice) const;
    [[nodiscard]] NativePortAudioEngineSnapshot snapshot() const;

  private:
    friend class NativePortSoundBankEngine;

    // Composite providers may refill a software voice several times during
    // one outer pump. The first pump samples the host master cursor; later
    // refill passes retire buffers and mix against that same sample.
    void pump_with_cached_playback_position();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime
