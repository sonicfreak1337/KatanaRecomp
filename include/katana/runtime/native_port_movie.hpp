#pragma once

#include "katana/runtime/native_port_audio.hpp"
#include "katana/runtime/native_port_codec.hpp"
#include "katana/runtime/native_port_ffmpeg_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_movie_contract_version = 1u;

enum class NativePortMovieState : std::uint8_t {
    Closed,
    Ready,
    Playing,
    Paused,
    Completed,
    Stopped,
    Failed,
};

enum class NativePortMovieFailure : std::uint8_t {
    None,
    InvalidConfig,
    ContentLoad,
    ContentIdentity,
    UnsupportedHost,
    HostTimeRegression,
    MissingRequiredStream,
    DecoderUnavailable,
    InvalidAudioBuffer,
    InvalidVideoBuffer,
    HostAudioFailure,
    HostMediaFailure,
    CodecProviderFailure,
    ThreadViolation,
    CallbackReentry,
};

class NativePortMovieError final : public std::runtime_error {
  public:
    NativePortMovieError(NativePortMovieFailure failure,
                         std::uint32_t platform_error_code,
                         std::string_view operation);
    [[nodiscard]] NativePortMovieFailure failure() const noexcept;
    [[nodiscard]] std::uint32_t platform_error_code() const noexcept;

  private:
    NativePortMovieFailure failure_;
    std::uint32_t platform_error_code_;
};

struct NativePortMovieVideoFrame final {
    std::uint64_t timestamp_nanoseconds = 0u;
    std::uint64_t duration_nanoseconds = 0u;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t stride_bytes = 0u;
    bool bottom_up = false;
    // BGRA8 in decoder storage order. The span remains valid only for the
    // callback duration; bottom_up tells the renderer which row is first.
    std::span<const std::byte> pixels;
    // Display aspect after applying decoder/container pixel-aspect metadata.
    // This is intentionally separate from the coded pixel extent: Sofdec and
    // other native assets commonly use non-square pixels.
    std::uint32_t display_aspect_numerator = 0u;
    std::uint32_t display_aspect_denominator = 0u;
};

using NativePortMovieVideoCallback = void (*)(void* user,
                                              const NativePortMovieVideoFrame& frame) noexcept;
using NativePortMovieStateCallback = void (*)(void* user,
                                              NativePortMovieState state,
                                              NativePortMovieFailure failure,
                                              std::uint32_t platform_error_code) noexcept;

struct NativePortMovieCallbacks final {
    void* user = nullptr;
    NativePortMovieVideoCallback video = nullptr;
    NativePortMovieStateCallback state = nullptr;
};

struct NativePortMovieSource final {
    std::filesystem::path content_root;
    std::filesystem::path content_relative_path;
    // Lowercase SHA-256 prefixed with "sha256:". The complete file is verified
    // through the same locked content handle later consumed by the decoder.
    std::string_view byte_identity;
};

struct NativePortMovieConfig final {
    NativePortMovieSource source;
    NativePortMovieCallbacks callbacks;
    // The built-in FFmpeg provider is the native product default and handles
    // Sofdec MPEG-PS/ADX in-process. Null explicitly selects Media Foundation.
    // A selected provider is exclusive; failures never fall back to MF.
    const NativePortCodecProvider* codec_provider = &native_port_ffmpeg_codec_provider();
    std::uint32_t audio_sample_rate = 44'100u;
    std::uint32_t maximum_audio_queue_frames = 44'100u;
    // Bound decoder-controlled host memory. A modest interleaved tail is
    // required when one multiplexed stream ends before the other; the byte
    // ceiling remains authoritative for large frames.
    std::uint32_t maximum_video_queue_frames = 64u;
    std::uint64_t maximum_video_frame_bytes = 64u * 1024u * 1024u;
    std::uint64_t maximum_video_queue_bytes = 128u * 1024u * 1024u;
    // Optional title/presentation override. Zero/zero consumes the decoder's
    // display aspect. This is required for formats such as Sofdec where the
    // game-side quad, rather than the MPEG stream, defines non-square pixels.
    std::uint32_t video_display_aspect_numerator = 0u;
    std::uint32_t video_display_aspect_denominator = 0u;
    bool require_audio = true;
    bool require_video = true;
};

struct NativePortMovieSnapshot final {
    NativePortMovieState state = NativePortMovieState::Closed;
    std::uint64_t duration_nanoseconds = 0u;
    std::uint64_t position_nanoseconds = 0u;
    std::uint64_t decoded_audio_frames = 0u;
    std::uint64_t decoded_video_frames = 0u;
    std::uint64_t presented_video_frames = 0u;
    NativePortMovieFailure failure = NativePortMovieFailure::None;
    std::uint32_t platform_error_code = 0u;
};

// Host movie decoding and presentation lifecycle.  Media timestamps and EOS
// are taken from the decoder; no guest player status is fabricated here.
// Construction, all operations and destruction are confined to one owner
// thread. Callbacks may inspect snapshot() or request stop(), but must not
// invoke other session operations recursively.
class NativePortMovieSession final {
  public:
    NativePortMovieSession();
    ~NativePortMovieSession();

    NativePortMovieSession(const NativePortMovieSession&) = delete;
    NativePortMovieSession& operator=(const NativePortMovieSession&) = delete;
    NativePortMovieSession(NativePortMovieSession&&) = delete;
    NativePortMovieSession& operator=(NativePortMovieSession&&) = delete;

    void open(const NativePortMovieConfig& config);
    void play(std::uint64_t host_time_nanoseconds);
    void pump(std::uint64_t host_time_nanoseconds);
    void pause(std::uint64_t host_time_nanoseconds);
    void stop();
    [[nodiscard]] NativePortMovieSnapshot snapshot() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime
