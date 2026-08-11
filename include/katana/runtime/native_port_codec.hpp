#pragma once

#include "katana/runtime/native_port_audio.hpp"

#include <cstddef>
#include <cstdint>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_codec_provider_contract_version = 2u;

// Read-only random access to the exact content object whose complete SHA-256
// identity was verified by the native runtime. Provider callbacks must not
// retain destination pointers beyond read_at().
struct NativePortCodecByteSource final {
    void* user = nullptr;
    std::uint64_t byte_size = 0u;
    std::uint32_t (*read_at)(void* user,
                             std::uint64_t offset,
                             void* destination,
                             std::uint64_t byte_count) noexcept = nullptr;
};

enum class NativePortCodecFailure : std::uint8_t {
    None,
    InvalidContract,
    UnsupportedContainer,
    UnsupportedCodec,
    InvalidData,
    ContentRead,
    ResourceExhausted,
    Internal,
};

enum class NativePortCodecSampleKind : std::uint8_t {
    Audio,
    Video,
    AudioEnd,
    VideoEnd,
};

struct NativePortCodecOpenRequest final {
    NativePortCodecByteSource source;
    std::uint32_t requested_audio_sample_rate = 44'100u;
    std::uint32_t maximum_audio_queue_frames = 44'100u;
    // MPEG-PS streams may end audio before video. The provider must retain the
    // bounded video tail until the demuxer can prove that no earlier audio
    // sample remains; 64 frames covers the verified Sofdec tail while the byte
    // budget remains the hard memory ceiling.
    std::uint32_t maximum_video_queue_frames = 64u;
    std::uint64_t maximum_video_frame_bytes = 64u * 1024u * 1024u;
    std::uint64_t maximum_video_queue_bytes = 128u * 1024u * 1024u;
    std::uint32_t require_audio = 1u;
    std::uint32_t require_video = 1u;
};

struct NativePortCodecOpenResult final {
    void* decoder = nullptr;
    std::uint64_t duration_nanoseconds = 0u;
    NativePortCodecFailure failure = NativePortCodecFailure::None;
    std::uint32_t provider_error_code = 0u;
    std::uint32_t audio_sample_rate = 0u;
    std::uint32_t audio_channels = 0u;
    std::uint32_t has_audio = 0u;
    std::uint32_t has_video = 0u;
};

struct NativePortCodecSample final {
    NativePortCodecSampleKind kind = NativePortCodecSampleKind::AudioEnd;
    std::uint64_t timestamp_nanoseconds = 0u;
    std::uint64_t duration_nanoseconds = 0u;

    std::uint32_t audio_sample_rate = 0u;
    std::uint32_t audio_channels = 0u;
    const std::int16_t* audio_samples = nullptr;
    std::uint64_t audio_sample_count = 0u;

    std::uint32_t video_width = 0u;
    std::uint32_t video_height = 0u;
    std::uint32_t video_stride_bytes = 0u;
    std::uint32_t video_bottom_up = 0u;
    const void* video_pixels = nullptr;
    std::uint64_t video_byte_count = 0u;
    // Display aspect after applying the source pixel/sample aspect ratio.
    // Both values must be non-zero for every video sample.
    std::uint32_t video_display_aspect_numerator = 0u;
    std::uint32_t video_display_aspect_denominator = 0u;
};

enum class NativePortCodecReadStatus : std::uint8_t {
    Sample,
    EndOfStream,
    Failure,
};

struct NativePortCodecReadResult final {
    NativePortCodecReadStatus status = NativePortCodecReadStatus::Failure;
    NativePortCodecSample sample;
    NativePortCodecFailure failure = NativePortCodecFailure::Internal;
    std::uint32_t provider_error_code = 0u;
};

// A title project links one immutable, static-lifetime provider table directly
// into its native adapter. structure_size must be set to
// native_port_codec_provider_structure_size. The ABI uses fixed-width scalar
// fields and pointer/out-parameters; no C++ library types cross the provider
// call boundary. No dynamic discovery, guest command translation or runtime
// codec fallback is performed. Sample pointers remain valid only until the next
// read_next() or close() call for the same decoder. Audio/video samples are
// globally timestamp-monotonic; each advertised stream ends exactly once via
// its End sample or the terminal EndOfStream result. Calls are synchronous on
// the movie session's owner thread. close() must join provider work and cease
// all ByteSource access before it returns.
struct NativePortCodecProvider final {
    std::uint32_t contract_version = native_port_codec_provider_contract_version;
    std::uint32_t structure_size = 0u;
    char provider_name[64]{};
    void* user = nullptr;
    void (*open)(void* user,
                 const NativePortCodecOpenRequest* request,
                 NativePortCodecOpenResult* result) noexcept = nullptr;
    void (*read_next)(void* decoder, NativePortCodecReadResult* result) noexcept = nullptr;
    void (*close)(void* decoder) noexcept = nullptr;
};

inline constexpr std::uint32_t native_port_codec_provider_structure_size =
    sizeof(NativePortCodecProvider);

[[nodiscard]] bool
valid_native_port_codec_provider(const NativePortCodecProvider& provider) noexcept;

} // namespace katana::runtime
