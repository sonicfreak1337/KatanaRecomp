#include "katana/runtime/native_port_audio_engine.hpp"

#include "native_port_audio_execution_domain.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

constexpr std::uint32_t maximum_native_audio_voices = 4'096u;
constexpr std::uint32_t maximum_mix_block_frames = 16'384u;
constexpr std::uint32_t maximum_decoder_reads_per_pump = 1'048'576u;
constexpr std::uint64_t maximum_codec_source_bytes =
    1ull * 1024ull * 1024ull * 1024ull;

[[nodiscard]] bool native_audio_signal_diagnostics_enabled() noexcept {
    static const bool enabled = [] {
        const auto* configured =
            std::getenv("KATANA_NATIVE_AUDIO_SIGNAL_DIAGNOSTICS");
        return configured != nullptr && std::string_view(configured) == "1";
    }();
    return enabled;
}

[[noreturn]] void fail_audio_engine(
    const NativePortAudioEngineFailure failure,
    const std::uint32_t provider_error_code,
    const std::string_view operation) {
    throw NativePortAudioEngineError(
        failure, provider_error_code, operation);
}

void saturating_add(std::uint64_t& destination,
                    const std::uint64_t value) noexcept {
    destination = value > std::numeric_limits<std::uint64_t>::max() - destination
                      ? std::numeric_limits<std::uint64_t>::max()
                      : destination + value;
}

[[nodiscard]] bool valid_gain_pan(const float gain, const float pan) noexcept {
    return std::isfinite(gain) && std::isfinite(pan) && gain >= 0.0f &&
           gain <= 4.0f && pan >= -1.0f && pan <= 1.0f;
}

void validate_engine_config(const NativePortAudioEngineConfig& config) {
    if (config.output_format.sample_rate < 8'000u ||
        config.output_format.sample_rate > 192'000u ||
        config.output_format.channels != 2u || config.maximum_voices == 0u ||
        config.maximum_voices > maximum_native_audio_voices ||
        config.maximum_output_queue_frames == 0u ||
        config.target_output_queue_frames == 0u ||
        config.target_output_queue_frames >
            config.maximum_output_queue_frames ||
        config.mix_block_frames == 0u ||
        config.mix_block_frames > maximum_mix_block_frames ||
        config.mix_block_frames > config.target_output_queue_frames ||
        config.maximum_buffered_frames_per_voice <
            config.mix_block_frames ||
        config.maximum_decoder_reads_per_pump == 0u ||
        config.maximum_decoder_reads_per_pump >
            maximum_decoder_reads_per_pump ||
        config.maximum_codec_source_bytes_per_voice == 0u ||
        config.maximum_codec_source_bytes_per_voice >
            maximum_codec_source_bytes)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                          0u,
                          "config");
}

[[nodiscard]] std::vector<std::byte> materialize_audio_content(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding,
    const std::uint64_t maximum_bytes) {
    if (binding.byte_size == 0u || binding.byte_size > maximum_bytes ||
        binding.byte_size > std::numeric_limits<std::size_t>::max())
        fail_audio_engine(NativePortAudioEngineFailure::ResourceLimit,
                          0u,
                          "content-budget");
    std::unique_ptr<NativePortReadOnlyFile> file;
    try {
        file = platform.open_content_file(binding);
    } catch (const NativePortPlatformError& error) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          error.platform_error_code(),
                          "content-open");
    } catch (...) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          0u,
                          "content-open");
    }
    if (file == nullptr || file->byte_size() != binding.byte_size)
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          0u,
                          "content-size");
    std::vector<std::byte> bytes(
        static_cast<std::size_t>(binding.byte_size));
    try {
        file->read_at(0u, bytes);
    } catch (const NativePortPlatformError& error) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          error.platform_error_code(),
                          "content-read");
    } catch (...) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          0u,
                          "content-read");
    }
    return bytes;
}

void validate_voice_config(const NativePortAudioVoiceConfig& config) {
    if (!valid_gain_pan(config.gain, config.pan) ||
        (!config.loop &&
         (config.loop_start_frame != 0u || config.loop_end_frame != 0u)) ||
        (config.loop_end_frame != 0u &&
         config.loop_start_frame >= config.loop_end_frame) ||
        (config.loop && config.loop_end_frame != 0u &&
         config.start_frame >= config.loop_end_frame))
        fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                          0u,
                          "voice-config");
}

[[nodiscard]] NativePortAudioEngineFailure map_codec_open_failure(
    const NativePortCodecFailure failure) noexcept {
    switch (failure) {
    case NativePortCodecFailure::ResourceExhausted:
        return NativePortAudioEngineFailure::ResourceLimit;
    case NativePortCodecFailure::None:
    case NativePortCodecFailure::InvalidContract:
    case NativePortCodecFailure::UnsupportedContainer:
    case NativePortCodecFailure::UnsupportedCodec:
    case NativePortCodecFailure::InvalidData:
    case NativePortCodecFailure::ContentRead:
    case NativePortCodecFailure::Internal:
        return NativePortAudioEngineFailure::DecoderOpen;
    }
    return NativePortAudioEngineFailure::DecoderOpen;
}

[[nodiscard]] NativePortAudioEngineFailure map_codec_read_failure(
    const NativePortCodecFailure failure) noexcept {
    return failure == NativePortCodecFailure::ResourceExhausted
               ? NativePortAudioEngineFailure::ResourceLimit
               : NativePortAudioEngineFailure::DecoderRead;
}

[[nodiscard]] std::uint16_t read_be16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2u)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-header-range");
    return static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(bytes[offset]))
               << 8u |
           static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 1u]));
}

[[nodiscard]] std::uint32_t read_be32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4u)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-header-range");
    return static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(bytes[offset]))
               << 24u |
           static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 1u]))
               << 16u |
           static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 2u]))
               << 8u |
           static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 3u]));
}

[[nodiscard]] std::uint64_t scale_audio_frame(
    const std::uint64_t frame,
    const std::uint32_t source_rate,
    const std::uint32_t output_rate) {
    const auto whole = frame / source_rate;
    const auto remainder = frame % source_rate;
    if (whole > std::numeric_limits<std::uint64_t>::max() / output_rate)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-frame-overflow");
    return whole * output_rate +
           (remainder * output_rate + source_rate / 2u) / source_rate;
}

enum class AudioEngineOpcode : std::uint16_t {
    Construct = 1u,
    Destroy,
    CreateVoice,
    CreatePcmFeed,
    SubmitPcm,
    FinishPcmFeed,
    Play,
    Pause,
    Resume,
    Stop,
    Release,
    SetGainPan,
    SetOutputPaused,
    StopAll,
    Pump,
    PumpCached,
    VoiceSnapshot,
    Snapshot,
};

struct AudioHandleCommand final {
    NativePortAudioVoiceHandle voice{};
};

struct AudioCreatePcmFeedCommand final {
    NativePortAudioVoiceConfig config{};
};

struct AudioSubmitPcmCommand final {
    NativePortAudioVoiceHandle voice{};
    std::uint32_t sample_count = 0u;
};

static_assert(sizeof(AudioSubmitPcmCommand) % alignof(std::int16_t) == 0u);

struct AudioGainPanCommand final {
    NativePortAudioVoiceHandle voice{};
    float gain = 1.0f;
    float pan = 0.0f;
};

struct AudioPauseOutputCommand final {
    std::uint8_t paused = 0u;
    std::array<std::uint8_t, 3u> reserved{};
};

struct AudioCreateVoiceCommand final {
    NativePortAudioVoiceConfig config{};
    std::uint64_t source_offset = 0u;
    std::uint64_t byte_size = 0u;
    std::uint32_t logical_id_size = 0u;
    std::uint32_t path_size = 0u;
    std::uint32_t byte_identity_size = 0u;
    std::uint32_t reserved = 0u;
};

struct AudioEngineWireError final {
    NativePortAudioEngineFailure failure =
        NativePortAudioEngineFailure::WorkerFailure;
    std::array<std::uint8_t, 3u> reserved{};
    std::uint32_t provider_error_code = 0u;
};

static_assert(std::is_trivially_copyable_v<AudioHandleCommand>);
static_assert(std::is_trivially_copyable_v<AudioCreatePcmFeedCommand>);
static_assert(std::is_trivially_copyable_v<AudioSubmitPcmCommand>);
static_assert(std::is_trivially_copyable_v<AudioGainPanCommand>);
static_assert(std::is_trivially_copyable_v<AudioPauseOutputCommand>);
static_assert(std::is_trivially_copyable_v<AudioCreateVoiceCommand>);
static_assert(std::is_trivially_copyable_v<AudioEngineWireError>);

template <typename Value>
[[nodiscard]] std::span<const std::byte> object_bytes(
    const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    return {reinterpret_cast<const std::byte*>(std::addressof(value)),
            sizeof(Value)};
}

template <typename Value>
[[nodiscard]] bool read_object(const std::span<const std::byte> bytes,
                               Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    if (bytes.size() != sizeof(Value)) return false;
    std::memcpy(std::addressof(value), bytes.data(), sizeof(Value));
    return true;
}

template <typename Value>
void write_ack_value(NativePortAudioCommandAckResult& result,
                     const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    static_assert(sizeof(Value) <=
                  native_port_audio_command_queue_max_ack_result_bytes);
    result.status = NativePortAudioCommandAckStatus::Completed;
    result.result_size = sizeof(Value);
    std::memcpy(result.bytes.data(), std::addressof(value), sizeof(Value));
}

void write_engine_error(NativePortAudioCommandAckResult& result,
                        const NativePortAudioEngineFailure failure,
                        const std::uint32_t provider_error_code) noexcept {
    const AudioEngineWireError error{failure, {}, provider_error_code};
    result.status = NativePortAudioCommandAckStatus::Failed;
    result.error_code = static_cast<std::uint32_t>(failure);
    result.result_size = sizeof(error);
    std::memcpy(result.bytes.data(), std::addressof(error), sizeof(error));
}

[[nodiscard]] bool checked_payload_sum(std::uint64_t& sum,
                                       const std::uint64_t addend) noexcept {
    if (addend > std::numeric_limits<std::uint64_t>::max() - sum) return false;
    sum += addend;
    return sum <= std::numeric_limits<std::uint32_t>::max();
}

} // namespace

NativePortAudioEngineError::NativePortAudioEngineError(
    const NativePortAudioEngineFailure failure,
    const std::uint32_t provider_error_code,
    const std::string_view operation)
    : std::runtime_error("native-port-audio-engine-" +
                         std::string(operation) + ":" +
                         std::to_string(provider_error_code)),
      failure_(failure),
      provider_error_code_(provider_error_code) {}

NativePortAudioEngineFailure
NativePortAudioEngineError::failure() const noexcept {
    return failure_;
}

std::uint32_t
NativePortAudioEngineError::provider_error_code() const noexcept {
    return provider_error_code_;
}

NativePortAdxStreamMetadata inspect_native_port_adx_content(
    NativePortPlatformServices& platform,
    const NativePortContentFileBinding& binding) {
    // The fixed ADX header ends at byte 24. Non-looping version-3 streams may
    // begin encoded data at byte 36; looping streams extend the metadata
    // through byte 43. Read the larger inspection prefix, but do not confuse
    // its size with the authored payload offset.
    constexpr std::size_t inspection_header_bytes = 44u;
    constexpr std::uint64_t minimum_data_start = 36u;
    constexpr std::uint64_t loop_metadata_end = 44u;
    std::unique_ptr<NativePortReadOnlyFile> file;
    try {
        file = platform.open_content_file(binding);
    } catch (const NativePortPlatformError& error) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          error.platform_error_code(),
                          "adx-content-open");
    } catch (...) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          0u,
                          "adx-content-open");
    }
    if (!file || file->byte_size() < inspection_header_bytes)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-content-size");

    std::array<std::byte, inspection_header_bytes> header{};
    try {
        file->read_at(0u, header);
    } catch (const NativePortPlatformError& error) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          error.platform_error_code(),
                          "adx-header-read");
    } catch (...) {
        fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                          0u,
                          "adx-header-read");
    }

    constexpr std::uint8_t adx_fixed_encoding = 3u;
    constexpr std::uint8_t adx_block_bytes = 18u;
    constexpr std::uint8_t adx_sample_bits = 4u;
    if (std::to_integer<std::uint8_t>(header[0]) != 0x80u ||
        std::to_integer<std::uint8_t>(header[1]) != 0x00u ||
        std::to_integer<std::uint8_t>(header[4]) != adx_fixed_encoding ||
        std::to_integer<std::uint8_t>(header[5]) != adx_block_bytes ||
        std::to_integer<std::uint8_t>(header[6]) != adx_sample_bits)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-header-format");

    NativePortAdxStreamMetadata metadata;
    metadata.channels = std::to_integer<std::uint8_t>(header[7]);
    metadata.sample_rate = read_be32(header, 8u);
    metadata.sample_frames = read_be32(header, 12u);
    if ((metadata.channels != 1u && metadata.channels != 2u) ||
        metadata.sample_rate < 8'000u || metadata.sample_rate > 192'000u ||
        metadata.sample_frames == 0u)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-stream-format");

    const auto data_start = static_cast<std::uint64_t>(read_be16(header, 2u)) + 4u;
    const auto blocks =
        (static_cast<std::uint64_t>(metadata.sample_frames) + 31u) / 32u;
    const auto encoded_bytes_per_block =
        static_cast<std::uint64_t>(adx_block_bytes) * metadata.channels;
    if (data_start < minimum_data_start ||
        blocks > std::numeric_limits<std::uint64_t>::max() /
                     encoded_bytes_per_block ||
        data_start > file->byte_size() ||
        blocks * encoded_bytes_per_block > file->byte_size() - data_start)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-payload-range");

    const auto loop_flag = read_be32(header, 24u);
    if (loop_flag > 1u)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "adx-loop-flag");
    if (loop_flag != 0u) {
        if (data_start < loop_metadata_end)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                              0u,
                              "adx-loop-header-range");
        metadata.loop_start_frame = read_be32(header, 28u);
        const auto loop_start_byte = read_be32(header, 32u);
        metadata.loop_end_frame = read_be32(header, 36u);
        const auto loop_end_byte = read_be32(header, 40u);
        if (metadata.loop_start_frame >= metadata.loop_end_frame ||
            metadata.loop_end_frame > metadata.sample_frames ||
            loop_start_byte < data_start || loop_start_byte >= loop_end_byte ||
            loop_end_byte > file->byte_size())
            fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                              0u,
                              "adx-loop-range");
        metadata.loop = true;
    }
    return metadata;
}

NativePortAudioVoiceConfig native_port_adx_voice_config(
    const NativePortAdxStreamMetadata& metadata,
    const std::uint32_t output_sample_rate,
    const float gain,
    const float pan) {
    if ((metadata.channels != 1u && metadata.channels != 2u) ||
        metadata.sample_rate == 0u || metadata.sample_frames == 0u ||
        output_sample_rate < 8'000u || output_sample_rate > 192'000u ||
        !valid_gain_pan(gain, pan) ||
        (metadata.loop &&
         (metadata.loop_start_frame >= metadata.loop_end_frame ||
          metadata.loop_end_frame > metadata.sample_frames)) ||
        (!metadata.loop &&
         (metadata.loop_start_frame != 0u || metadata.loop_end_frame != 0u)))
        fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                          0u,
                          "adx-voice-config");

    NativePortAudioVoiceConfig config;
    config.gain = gain;
    config.pan = pan;
    config.loop = metadata.loop;
    if (metadata.loop) {
        config.loop_start_frame = scale_audio_frame(
            metadata.loop_start_frame, metadata.sample_rate, output_sample_rate);
        config.loop_end_frame = scale_audio_frame(
            metadata.loop_end_frame, metadata.sample_rate, output_sample_rate);
        if (config.loop_start_frame >= config.loop_end_frame)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                              0u,
                              "adx-loop-scale");
    }
    return config;
}

class NativePortAudioEngine::Core final {
  private:
    struct Voice;

    class PendingDecoderOwnership final {
      public:
        PendingDecoderOwnership(Core& owner, Voice& voice) noexcept
            : owner_(owner), voice_(voice) {}

        ~PendingDecoderOwnership() noexcept {
            if (armed_) owner_.close_decoder(voice_);
        }

        PendingDecoderOwnership(const PendingDecoderOwnership&) = delete;
        PendingDecoderOwnership& operator=(
            const PendingDecoderOwnership&) = delete;

        void disarm() noexcept { armed_ = false; }

      private:
        Core& owner_;
        Voice& voice_;
        bool armed_ = true;
    };

  public:
    Core(const NativePortCodecProvider& codec_provider,
         const NativePortAudioEngineConfig& config)
        : codec_provider_(codec_provider), config_(config),
          owner_thread_(std::this_thread::get_id()) {
        validate_engine_config(config_);
        if (!valid_native_port_codec_provider(codec_provider_))
            fail_audio_engine(NativePortAudioEngineFailure::InvalidProvider,
                              0u,
                              "provider");
        try {
            output_ = std::make_unique<NativePortAudioStream>(
                NativePortAudioConfig{config_.output_format,
                                      config_.maximum_output_queue_frames,
                                      config_.command_queue});
        } catch (...) {
            fail_audio_engine(NativePortAudioEngineFailure::HostAudio,
                              0u,
                              "output-open");
        }
        mix_accumulation_.resize(
            static_cast<std::size_t>(config_.mix_block_frames) * 2u);
        mixed_samples_.resize(
            static_cast<std::size_t>(config_.mix_block_frames) * 2u);
        pending_contributions_.reserve(config_.maximum_voices);
        slots_.reserve(config_.maximum_voices);
    }

    ~Core() {
        for (auto& slot : slots_) {
            if (slot.voice) close_decoder(*slot.voice);
        }
        if (output_) {
            try {
                output_->stop();
            } catch (...) {
            }
        }
    }

    [[nodiscard]] NativePortAudioVoiceHandle create_voice(
        const NativePortContentFileBinding& binding,
        std::vector<std::byte> content,
        const NativePortAudioVoiceConfig& config) {
        require_owner_thread();
        validate_voice_config(config);

        auto voice = std::make_unique<Voice>();
        voice->owner = this;
        voice->source = NativePortAudioVoiceSource::Codec;
        voice->config = config;
        voice->logical_id = std::string(binding.logical_id);
        voice->content_relative_path = binding.content_relative_path;
        voice->byte_identity = std::string(binding.byte_identity);
        voice->source_offset = binding.source_offset;
        voice->byte_size = binding.byte_size;
        if (content.size() != binding.byte_size)
            fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                              0u,
                              "content-snapshot");
        voice->content = std::move(content);
        reserve_voice_storage(*voice);
        open_decoder(*voice, voice->config.start_frame);
        // open_decoder() transfers the provider handle into Voice before the
        // slot vector may need to grow. Keep that handle guarded until the
        // unique_ptr has reached a slot; vector allocation can throw while
        // the Voice destructor itself deliberately has no provider coupling.
        PendingDecoderOwnership pending_decoder(*this, *voice);
        const auto handle = insert_voice(std::move(voice));
        pending_decoder.disarm();
        return handle;
    }

    [[nodiscard]] NativePortAudioVoiceHandle create_pcm_feed(
        const NativePortAudioVoiceConfig& config) {
        require_owner_thread();
        validate_voice_config(config);
        if (config.loop || config.start_frame != 0u ||
            config.loop_start_frame != 0u || config.loop_end_frame != 0u)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                              0u,
                              "feed-config");
        auto voice = std::make_unique<Voice>();
        voice->owner = this;
        voice->source = NativePortAudioVoiceSource::PcmFeed;
        voice->config = config;
        voice->channels = 2u;
        reserve_voice_storage(*voice);
        return insert_voice(std::move(voice));
    }

    [[nodiscard]] bool submit_pcm_s16(
        const NativePortAudioVoiceHandle handle,
        const std::span<const std::int16_t> samples) {
        require_owner_thread();
        auto& voice = require_voice(handle);
        if (voice.source != NativePortAudioVoiceSource::PcmFeed)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                              0u,
                              "feed-source");
        if (voice.state == NativePortAudioVoiceState::Failed ||
            voice.state == NativePortAudioVoiceState::Completed ||
            voice.state == NativePortAudioVoiceState::Stopped ||
            voice.feed_finished)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                              0u,
                              "feed-closed");
        if (samples.empty() || (samples.size() & 1u) != 0u)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                              0u,
                              "feed-layout");
        const auto frames = samples.size() / 2u;
        compact_pcm(voice);
        if (frames > config_.maximum_buffered_frames_per_voice ||
            available_frames(voice) >
                config_.maximum_buffered_frames_per_voice - frames)
            return false;
        voice.pcm.insert(voice.pcm.end(), samples.begin(), samples.end());
        saturating_add(voice.decoded_source_frames, frames);
        saturating_add(decoded_source_frames_, frames);
        saturating_add(submitted_feed_frames_, frames);
        return true;
    }

    void finish_pcm_feed(const NativePortAudioVoiceHandle handle) {
        require_owner_thread();
        auto& voice = require_voice(handle);
        if (voice.source != NativePortAudioVoiceSource::PcmFeed)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                              0u,
                              "feed-source");
        voice.feed_finished = true;
        if (voice.state == NativePortAudioVoiceState::Playing &&
            available_frames(voice) == 0u)
            voice.state = NativePortAudioVoiceState::Completed;
    }

    // The rvalue reference deliberately keeps ownership in the caller until
    // every potentially allocating slot operation has completed.
    [[nodiscard]] NativePortAudioVoiceHandle insert_voice(
        std::unique_ptr<Voice>&& voice) {
        std::size_t slot_index = slots_.size();
        for (std::size_t index = 0u; index < slots_.size(); ++index) {
            if (!slots_[index].voice) {
                slot_index = index;
                break;
            }
        }
        if (slot_index == slots_.size()) {
            if (slots_.size() == config_.maximum_voices) {
                fail_audio_engine(NativePortAudioEngineFailure::ResourceLimit,
                                  0u,
                                  "voice-limit");
            }
            slots_.push_back(Slot{});
        }
        auto& slot = slots_[slot_index];
        slot.voice = std::move(voice);
        saturating_add(created_voices_, 1u);
        return {static_cast<std::uint32_t>(slot_index), slot.generation};
    }

    void play(const NativePortAudioVoiceHandle handle) {
        require_owner_thread();
        auto& voice = require_voice(handle);
        if (voice.state == NativePortAudioVoiceState::Failed)
            fail_audio_engine(voice.failure,
                              voice.provider_error_code,
                              "voice-failed");
        if (voice.state == NativePortAudioVoiceState::Playing) return;
        if (voice.state == NativePortAudioVoiceState::Paused) {
            voice.state = NativePortAudioVoiceState::Playing;
            return;
        }
        if (voice.state == NativePortAudioVoiceState::Completed ||
            voice.state == NativePortAudioVoiceState::Stopped) {
            if (voice.source == NativePortAudioVoiceSource::Codec) {
                reset_voice(voice, voice.config.start_frame);
            } else {
                voice.pcm.clear();
                voice.read_frame = 0u;
                voice.feed_finished = false;
                voice.state = NativePortAudioVoiceState::Ready;
            }
        }
        voice.state = NativePortAudioVoiceState::Playing;
    }

    void pause(const NativePortAudioVoiceHandle handle) {
        require_owner_thread();
        auto& voice = require_voice(handle);
        if (voice.state == NativePortAudioVoiceState::Playing)
            voice.state = NativePortAudioVoiceState::Paused;
    }

    void resume(const NativePortAudioVoiceHandle handle) {
        require_owner_thread();
        auto& voice = require_voice(handle);
        if (voice.state == NativePortAudioVoiceState::Paused)
            voice.state = NativePortAudioVoiceState::Playing;
    }

    void stop(const NativePortAudioVoiceHandle handle) {
        require_owner_thread();
        auto& voice = require_voice(handle);
        close_decoder(voice);
        voice.pcm.clear();
        voice.read_frame = 0u;
        voice.feed_finished = false;
        voice.state = NativePortAudioVoiceState::Stopped;
    }

    void release(const NativePortAudioVoiceHandle handle) {
        require_owner_thread();
        auto& slot = require_slot(handle);
        close_decoder(*slot.voice);
        slot.voice.reset();
        ++slot.generation;
        if (slot.generation == 0u) slot.generation = 1u;
        saturating_add(released_voices_, 1u);
    }

    void set_gain_pan(const NativePortAudioVoiceHandle handle,
                      const float gain,
                      const float pan) {
        require_owner_thread();
        if (!valid_gain_pan(gain, pan))
            fail_audio_engine(
                NativePortAudioEngineFailure::InvalidVoiceConfig,
                0u,
                "gain-pan");
        auto& voice = require_voice(handle);
        voice.config.gain = gain;
        voice.config.pan = pan;
    }

    void set_output_paused(const bool paused) {
        require_owner_thread();
        if (output_paused_ == paused) return;
        try {
            if (paused)
                output_->pause();
            else
                output_->resume();
        } catch (...) {
            fail_audio_engine(NativePortAudioEngineFailure::HostAudio,
                              0u,
                              paused ? "output-pause" : "output-resume");
        }
        output_paused_ = paused;
    }

    void stop_all() {
        require_owner_thread();
        for (auto& slot : slots_) {
            if (!slot.voice) continue;
            close_decoder(*slot.voice);
            slot.voice->pcm.clear();
            slot.voice->read_frame = 0u;
            slot.voice->feed_finished = false;
            slot.voice->state = NativePortAudioVoiceState::Stopped;
        }
    }

    void pump(const bool refresh_playback_position) {
        require_owner_thread();
        try {
            output_->poll();
            if (refresh_playback_position) {
                output_->refresh_playback_position();
                retire_played_output_segments(
                    output_->playback_position_frames());
            }
        } catch (...) {
            fail_audio_engine(NativePortAudioEngineFailure::HostAudio,
                              0u,
                              "output-poll");
        }
        if (output_paused_) return;

        auto output_snapshot = output_->snapshot();
        if (output_snapshot.state == NativePortAudioState::Failed)
            fail_audio_engine(NativePortAudioEngineFailure::HostAudio,
                              output_snapshot.error_code,
                              "output-state");

        // WAVEHDR ownership is released only after a complete submitted block
        // finishes.  NativePortAudioSnapshot::queued_frames is consequently a
        // conservative storage/lifetime count and can overstate the samples
        // the device still has left to play by almost one mix block.  At a
        // 60-Hz service boundary that error consumed nearly the entire jitter
        // margin and produced periodic endpoint underruns.  Drive the mixer
        // from the monotone device sample cursor instead; submitted frames are
        // still the authoritative upper bound if a host driver reports a
        // stale or rounded position.
        const auto remaining_output_frames = [&]() noexcept {
            const auto played = std::min(
                output_snapshot.submitted_frames,
                output_->playback_position_frames());
            return output_snapshot.submitted_frames - played;
        };

        std::uint32_t decoder_reads_remaining =
            config_.maximum_decoder_reads_per_pump;
        while (remaining_output_frames() <
               config_.target_output_queue_frames) {
            const auto queue_room =
                config_.target_output_queue_frames -
                static_cast<std::uint32_t>(remaining_output_frames());
            const auto requested_frames =
                std::min(config_.mix_block_frames, queue_room);
            const auto output_start_frame = output_snapshot.submitted_frames;
            const auto mixed_frames =
                mix_block(requested_frames, decoder_reads_remaining);
            if (mixed_frames == 0u) break;
            const auto sample_count =
                static_cast<std::size_t>(mixed_frames) * 2u;
            bool submitted = false;
            try {
                stage_output_segments(output_start_frame);
                submitted = output_->submit_pcm_s16(
                    std::span<const std::int16_t>(mixed_samples_.data(),
                                                  sample_count));
            } catch (...) {
                rollback_staged_output_segments();
                fail_audio_engine(NativePortAudioEngineFailure::HostAudio,
                                  0u,
                                  "output-submit");
            }
            if (!submitted) {
                rollback_staged_output_segments();
                break;
            }
            pending_contributions_.clear();
            saturating_add(submitted_output_frames_, mixed_frames);
            if (native_audio_signal_diagnostics_enabled()) {
                saturating_add(submitted_nonzero_samples_,
                               pending_nonzero_samples_);
                saturating_add(submitted_clipped_samples_,
                               pending_clipped_samples_);
                submitted_peak_sample_ = std::max(
                    submitted_peak_sample_, pending_peak_sample_);
            }
            output_snapshot = output_->snapshot();
        }
    }

    [[nodiscard]] NativePortAudioVoiceSnapshot voice_snapshot(
        const NativePortAudioVoiceHandle handle) const {
        require_owner_thread();
        const auto& voice = require_voice(handle);
        return {voice.source,
                voice.state,
                voice.duration_nanoseconds,
                voice.decoded_source_frames,
                voice.mixed_output_frames,
                voice.played_output_frames,
                available_frames(voice),
                voice.loop_count,
                voice.config.gain,
                voice.config.pan,
                voice.failure,
                voice.provider_error_code};
    }

    [[nodiscard]] NativePortAudioEngineSnapshot snapshot() const {
        require_owner_thread();
        NativePortAudioEngineSnapshot result;
        result.created_voices = created_voices_;
        result.released_voices = released_voices_;
        result.decoder_reads = decoder_reads_;
        result.decoded_source_frames = decoded_source_frames_;
        result.submitted_feed_frames = submitted_feed_frames_;
        result.mixed_output_frames = mixed_output_frames_;
        result.submitted_output_frames = submitted_output_frames_;
        result.submitted_nonzero_samples = submitted_nonzero_samples_;
        result.submitted_clipped_samples = submitted_clipped_samples_;
        result.submitted_peak_sample = submitted_peak_sample_;
        result.output_paused = output_paused_;
        result.output = output_->snapshot();
        for (const auto& slot : slots_) {
            if (!slot.voice) continue;
            ++result.active_voices;
            if (slot.voice->state == NativePortAudioVoiceState::Playing)
                ++result.playing_voices;
            if (slot.voice->state == NativePortAudioVoiceState::Failed)
                ++result.failed_voices;
        }
        return result;
    }

  private:
    struct OutputSegment final {
        std::uint64_t start_frame = 0u;
        std::uint32_t frames = 0u;
    };

    struct Voice final {
        Core* owner = nullptr;
        NativePortAudioVoiceSource source =
            NativePortAudioVoiceSource::Codec;
        std::string logical_id;
        std::filesystem::path content_relative_path;
        std::string byte_identity;
        std::uint64_t source_offset = 0u;
        std::uint64_t byte_size = 0u;
        NativePortAudioVoiceConfig config;
        std::vector<std::byte> content;
        void* decoder = nullptr;
        NativePortAudioVoiceState state = NativePortAudioVoiceState::Ready;
        NativePortAudioEngineFailure failure =
            NativePortAudioEngineFailure::None;
        std::uint32_t provider_error_code = 0u;
        std::uint32_t channels = 0u;
        std::uint64_t duration_nanoseconds = 0u;
        std::uint64_t source_frame = 0u;
        std::uint64_t discard_before_frame = 0u;
        std::uint64_t decoded_source_frames = 0u;
        std::uint64_t mixed_output_frames = 0u;
        std::uint64_t played_output_frames = 0u;
        std::uint64_t loop_count = 0u;
        std::uint64_t frames_since_restart = 0u;
        std::uint32_t consecutive_empty_loops = 0u;
        std::vector<std::int16_t> pcm;
        std::vector<OutputSegment> output_segments;
        std::size_t output_segment_head = 0u;
        std::size_t output_segment_count = 0u;
        std::size_t read_frame = 0u;
        bool feed_finished = false;

        [[nodiscard]] NativePortContentFileBinding binding() const {
            return {logical_id,
                    content_relative_path,
                    byte_identity,
                    source_offset,
                    byte_size};
        }
    };

    struct Slot final {
        std::uint32_t generation = 1u;
        std::unique_ptr<Voice> voice;
    };

    struct PendingContribution final {
        Voice* voice = nullptr;
        std::uint32_t frames = 0u;
    };

    void reserve_voice_storage(Voice& voice) {
        const auto maximum_frames = static_cast<std::size_t>(
            config_.maximum_buffered_frames_per_voice);
        if (maximum_frames >
            std::numeric_limits<std::size_t>::max() / 2u)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u,
                              "voice-storage");
        voice.pcm.reserve(maximum_frames * 2u);
        // Each outstanding segment owns at least one output frame, therefore
        // maximum_output_queue_frames is a strict upper bound on live segment
        // records. Resize once at voice creation; mix/poll only move ring
        // indices and never allocate.
        voice.output_segments.resize(
            config_.maximum_output_queue_frames);
    }

    void retire_played_output_segments(
        const std::uint64_t playback_position) noexcept {
        for (auto& slot : slots_) {
            if (!slot.voice) continue;
            auto& voice = *slot.voice;
            while (voice.output_segment_count != 0u) {
                auto& segment = voice.output_segments[
                    voice.output_segment_head];
                if (playback_position <= segment.start_frame) break;
                const auto consumed = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        segment.frames,
                        playback_position - segment.start_frame));
                saturating_add(voice.played_output_frames, consumed);
                segment.start_frame += consumed;
                segment.frames -= consumed;
                if (segment.frames != 0u) break;
                voice.output_segment_head =
                    (voice.output_segment_head + 1u) %
                    voice.output_segments.size();
                --voice.output_segment_count;
            }
        }
    }

    void stage_output_segments(const std::uint64_t output_start_frame) {
        std::size_t staged = 0u;
        try {
            for (const auto& contribution : pending_contributions_) {
                auto& voice = *contribution.voice;
                if (voice.output_segment_count ==
                    voice.output_segments.size())
                    fail_audio_engine(
                        NativePortAudioEngineFailure::ResourceLimit,
                        0u,
                        "output-segment-budget");
                const auto tail =
                    (voice.output_segment_head +
                     voice.output_segment_count) %
                    voice.output_segments.size();
                voice.output_segments[tail] = {
                    output_start_frame, contribution.frames};
                ++voice.output_segment_count;
                ++staged;
            }
        } catch (...) {
            while (staged != 0u) {
                --staged;
                --pending_contributions_[staged]
                      .voice->output_segment_count;
            }
            pending_contributions_.clear();
            throw;
        }
    }

    void rollback_staged_output_segments() noexcept {
        for (auto contribution = pending_contributions_.rbegin();
             contribution != pending_contributions_.rend();
             ++contribution)
            --contribution->voice->output_segment_count;
        pending_contributions_.clear();
    }

    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_)
            fail_audio_engine(NativePortAudioEngineFailure::ThreadViolation,
                              0u,
                              "thread");
    }

    [[nodiscard]] Slot& require_slot(
        const NativePortAudioVoiceHandle handle) {
        if (!handle || handle.slot >= slots_.size() ||
            slots_[handle.slot].generation != handle.generation ||
            !slots_[handle.slot].voice)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidHandle,
                              0u,
                              "voice-handle");
        return slots_[handle.slot];
    }

    [[nodiscard]] const Slot& require_slot(
        const NativePortAudioVoiceHandle handle) const {
        if (!handle || handle.slot >= slots_.size() ||
            slots_[handle.slot].generation != handle.generation ||
            !slots_[handle.slot].voice)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidHandle,
                              0u,
                              "voice-handle");
        return slots_[handle.slot];
    }

    [[nodiscard]] Voice& require_voice(
        const NativePortAudioVoiceHandle handle) {
        return *require_slot(handle).voice;
    }

    [[nodiscard]] const Voice& require_voice(
        const NativePortAudioVoiceHandle handle) const {
        return *require_slot(handle).voice;
    }

    static std::uint32_t provider_read_at(
        void* const user,
        const std::uint64_t offset,
        void* const destination,
        const std::uint64_t byte_count) noexcept {
        auto& voice = *static_cast<Voice*>(user);
        if (destination == nullptr || offset > voice.content.size() ||
            byte_count > voice.content.size() - offset ||
            byte_count > std::numeric_limits<std::size_t>::max())
            return 0u;
        std::memcpy(destination,
                    voice.content.data() + offset,
                    static_cast<std::size_t>(byte_count));
        return 1u;
    }

    void mark_voice_failure(Voice& voice,
                            const NativePortAudioEngineFailure failure,
                            const std::uint32_t provider_error_code) noexcept {
        voice.failure = failure;
        voice.provider_error_code = provider_error_code;
        voice.state = NativePortAudioVoiceState::Failed;
        close_decoder(voice);
    }

    void open_decoder(Voice& voice, const std::uint64_t discard_before) {
        if (voice.decoder != nullptr)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidVoiceConfig,
                              0u,
                              "decoder-already-open");
        NativePortCodecOpenRequest request;
        request.source = {
            &voice, voice.content.size(), &Core::provider_read_at};
        request.requested_audio_sample_rate = config_.output_format.sample_rate;
        request.maximum_audio_queue_frames =
            config_.maximum_buffered_frames_per_voice;
        // Codec providers still validate the complete bounded request even
        // when video is not required: an input may advertise an unexpected
        // video stream which the audio engine then rejects below. Keep the
        // unused surface at the provider's smallest representable BGRA pixel
        // instead of passing an internally invalid one-byte budget.
        request.maximum_video_queue_frames = 1u;
        request.maximum_video_frame_bytes = 4u;
        request.maximum_video_queue_bytes = 4u;
        request.require_audio = 1u;
        request.require_video = 0u;

        NativePortCodecOpenResult result;
        codec_provider_.open(codec_provider_.user, &request, &result);
        if (result.has_audio > 1u || result.has_video > 1u) {
            if (result.decoder != nullptr)
                codec_provider_.close(result.decoder);
            fail_audio_engine(NativePortAudioEngineFailure::DecoderOpen,
                              result.provider_error_code,
                              "decoder-flags");
        }
        if (result.failure != NativePortCodecFailure::None ||
            result.decoder == nullptr || result.has_audio == 0u) {
            if (result.decoder != nullptr)
                codec_provider_.close(result.decoder);
            fail_audio_engine(map_codec_open_failure(result.failure),
                              result.provider_error_code,
                              "decoder-open");
        }
        if (result.has_video != 0u) {
            codec_provider_.close(result.decoder);
            fail_audio_engine(NativePortAudioEngineFailure::UnexpectedVideo,
                              result.provider_error_code,
                              "decoder-video-stream");
        }
        if (result.audio_sample_rate != config_.output_format.sample_rate ||
            (result.audio_channels != 1u && result.audio_channels != 2u)) {
            codec_provider_.close(result.decoder);
            fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                              result.provider_error_code,
                              "decoder-format");
        }
        voice.decoder = result.decoder;
        voice.channels = result.audio_channels;
        voice.duration_nanoseconds = result.duration_nanoseconds;
        voice.source_frame = 0u;
        voice.discard_before_frame = discard_before;
        voice.frames_since_restart = 0u;
    }

    void close_decoder(Voice& voice) noexcept {
        if (voice.decoder == nullptr) return;
        codec_provider_.close(voice.decoder);
        voice.decoder = nullptr;
    }

    void reset_voice(Voice& voice, const std::uint64_t discard_before) {
        close_decoder(voice);
        voice.pcm.clear();
        voice.read_frame = 0u;
        voice.failure = NativePortAudioEngineFailure::None;
        voice.provider_error_code = 0u;
        voice.consecutive_empty_loops = 0u;
        try {
            open_decoder(voice, discard_before);
        } catch (const NativePortAudioEngineError& error) {
            voice.failure = error.failure();
            voice.provider_error_code = error.provider_error_code();
            voice.state = NativePortAudioVoiceState::Failed;
            throw;
        }
        voice.state = NativePortAudioVoiceState::Ready;
    }

    void restart_loop(Voice& voice) {
        if (!voice.config.loop) {
            close_decoder(voice);
            voice.state = NativePortAudioVoiceState::Completed;
            return;
        }
        if (voice.frames_since_restart == 0u) {
            ++voice.consecutive_empty_loops;
            if (voice.consecutive_empty_loops > 1u) {
                mark_voice_failure(voice,
                                   NativePortAudioEngineFailure::InvalidAudioBuffer,
                                   0u);
                fail_audio_engine(
                    NativePortAudioEngineFailure::InvalidAudioBuffer,
                    0u,
                    "empty-loop");
            }
        } else {
            voice.consecutive_empty_loops = 0u;
        }
        close_decoder(voice);
        try {
            open_decoder(voice, voice.config.loop_start_frame);
        } catch (const NativePortAudioEngineError& error) {
            voice.failure = error.failure();
            voice.provider_error_code = error.provider_error_code();
            voice.state = NativePortAudioVoiceState::Failed;
            throw;
        }
        saturating_add(voice.loop_count, 1u);
    }

    [[nodiscard]] static std::uint64_t
    available_frames(const Voice& voice) noexcept {
        const auto frames = voice.pcm.size() / 2u;
        return frames >= voice.read_frame ? frames - voice.read_frame : 0u;
    }

    void compact_pcm(Voice& voice) {
        if (voice.read_frame == 0u) return;
        const auto total_frames = voice.pcm.size() / 2u;
        if (voice.read_frame < 4'096u &&
            voice.read_frame * 2u < total_frames)
            return;
        const auto consumed_samples = voice.read_frame * 2u;
        voice.pcm.erase(voice.pcm.begin(),
                        voice.pcm.begin() +
                            static_cast<std::ptrdiff_t>(consumed_samples));
        voice.read_frame = 0u;
    }

    void append_audio_sample(Voice& voice,
                             const NativePortCodecSample& sample) {
        if (sample.audio_sample_rate != config_.output_format.sample_rate ||
            sample.audio_channels != voice.channels ||
            (sample.audio_channels != 1u && sample.audio_channels != 2u) ||
            sample.audio_samples == nullptr ||
            sample.audio_sample_count == 0u ||
            sample.audio_sample_count % sample.audio_channels != 0u) {
            mark_voice_failure(voice,
                               NativePortAudioEngineFailure::InvalidAudioBuffer,
                               0u);
            fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                              0u,
                              "audio-sample");
        }
        const auto sample_frames =
            sample.audio_sample_count / sample.audio_channels;
        if (sample_frames > config_.maximum_buffered_frames_per_voice) {
            mark_voice_failure(voice,
                               NativePortAudioEngineFailure::ResourceLimit,
                               0u);
            fail_audio_engine(NativePortAudioEngineFailure::ResourceLimit,
                              0u,
                              "audio-sample-budget");
        }
        compact_pcm(voice);

        // FFmpeg commonly hands the native ADX path a complete stereo batch.
        // Keep boundary-sensitive loop/discard/error handling below, but copy
        // the ordinary contiguous batch at once and aggregate its monotone
        // counters instead of paying two vector appends and three saturating
        // additions for every decoded frame.
        const auto complete_source_range =
            sample_frames <=
            std::numeric_limits<std::uint64_t>::max() - voice.source_frame;
        const auto before_loop_boundary =
            !voice.config.loop || voice.config.loop_end_frame == 0u ||
            (voice.source_frame < voice.config.loop_end_frame &&
             sample_frames <=
                 voice.config.loop_end_frame - voice.source_frame);
        const auto available = available_frames(voice);
        if (sample.audio_channels == 2u && complete_source_range &&
            before_loop_boundary &&
            voice.source_frame >= voice.discard_before_frame &&
            sample_frames <= config_.maximum_buffered_frames_per_voice &&
            available <=
                config_.maximum_buffered_frames_per_voice - sample_frames) {
            voice.pcm.insert(
                voice.pcm.end(), sample.audio_samples,
                sample.audio_samples + sample.audio_sample_count);
            voice.source_frame += sample_frames;
            saturating_add(voice.frames_since_restart, sample_frames);
            saturating_add(voice.decoded_source_frames, sample_frames);
            saturating_add(decoded_source_frames_, sample_frames);
            return;
        }

        for (std::uint64_t frame = 0u; frame < sample_frames; ++frame) {
            if (voice.config.loop && voice.config.loop_end_frame != 0u &&
                voice.source_frame >= voice.config.loop_end_frame) {
                restart_loop(voice);
                break;
            }

            const auto source_index =
                static_cast<std::size_t>(frame * sample.audio_channels);
            if (voice.source_frame >= voice.discard_before_frame) {
                if (available_frames(voice) >=
                    config_.maximum_buffered_frames_per_voice) {
                    mark_voice_failure(
                        voice,
                        NativePortAudioEngineFailure::ResourceLimit,
                        0u);
                    fail_audio_engine(
                        NativePortAudioEngineFailure::ResourceLimit,
                        0u,
                        "voice-buffer-budget");
                }
                const auto left = sample.audio_samples[source_index];
                const auto right = sample.audio_channels == 2u
                                       ? sample.audio_samples[source_index + 1u]
                                       : left;
                voice.pcm.push_back(left);
                voice.pcm.push_back(right);
                saturating_add(voice.frames_since_restart, 1u);
            }
            ++voice.source_frame;
            saturating_add(voice.decoded_source_frames, 1u);
            saturating_add(decoded_source_frames_, 1u);
        }
    }

    void fill_voice(Voice& voice,
                    const std::uint32_t requested_frames,
                    std::uint32_t& decoder_reads_remaining) {
        if (voice.source == NativePortAudioVoiceSource::PcmFeed) {
            if (voice.feed_finished && available_frames(voice) == 0u)
                voice.state = NativePortAudioVoiceState::Completed;
            return;
        }
        while (voice.state == NativePortAudioVoiceState::Playing &&
               available_frames(voice) < requested_frames &&
               decoder_reads_remaining != 0u) {
            if (voice.decoder == nullptr) {
                mark_voice_failure(voice,
                                   NativePortAudioEngineFailure::DecoderRead,
                                   0u);
                fail_audio_engine(NativePortAudioEngineFailure::DecoderRead,
                                  0u,
                                  "decoder-closed");
            }
            NativePortCodecReadResult result;
            codec_provider_.read_next(voice.decoder, &result);
            --decoder_reads_remaining;
            saturating_add(decoder_reads_, 1u);
            if (result.status == NativePortCodecReadStatus::Failure) {
                const auto failure = map_codec_read_failure(result.failure);
                mark_voice_failure(
                    voice, failure, result.provider_error_code);
                fail_audio_engine(
                    failure, result.provider_error_code, "decoder-read");
            }
            if (result.status == NativePortCodecReadStatus::EndOfStream) {
                restart_loop(voice);
                continue;
            }
            switch (result.sample.kind) {
            case NativePortCodecSampleKind::Audio:
                append_audio_sample(voice, result.sample);
                break;
            case NativePortCodecSampleKind::AudioEnd:
                restart_loop(voice);
                break;
            case NativePortCodecSampleKind::Video:
            case NativePortCodecSampleKind::VideoEnd:
                mark_voice_failure(voice,
                                   NativePortAudioEngineFailure::UnexpectedVideo,
                                   result.provider_error_code);
                fail_audio_engine(NativePortAudioEngineFailure::UnexpectedVideo,
                                  result.provider_error_code,
                                  "decoder-video-sample");
            }
        }
    }

    [[nodiscard]] std::uint32_t mix_block(
        const std::uint32_t requested_frames,
        std::uint32_t& decoder_reads_remaining) {
        pending_contributions_.clear();
        bool has_live_voice = false;
        std::uint64_t maximum_available = 0u;
        for (auto& slot : slots_) {
            if (!slot.voice) continue;
            auto& voice = *slot.voice;
            if (voice.state == NativePortAudioVoiceState::Playing) {
                has_live_voice = true;
                fill_voice(voice, requested_frames, decoder_reads_remaining);
                // Do not advance other voices while one live decoder has not
                // produced the requested clock interval within this pump's
                // bounded work budget.
                if (voice.source == NativePortAudioVoiceSource::Codec &&
                    voice.state == NativePortAudioVoiceState::Playing &&
                    available_frames(voice) < requested_frames)
                    return 0u;
            }
            if (voice.state == NativePortAudioVoiceState::Playing ||
                voice.state == NativePortAudioVoiceState::Completed)
                maximum_available =
                    std::max(maximum_available, available_frames(voice));
        }
        if (!has_live_voice && maximum_available == 0u) return 0u;
        const auto frames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(requested_frames, maximum_available));
        if (frames == 0u) return 0u;

        const auto sample_count = static_cast<std::size_t>(frames) * 2u;
        std::fill_n(mix_accumulation_.begin(), sample_count, 0.0);
        for (auto& slot : slots_) {
            if (!slot.voice) continue;
            auto& voice = *slot.voice;
            if (voice.state != NativePortAudioVoiceState::Playing &&
                voice.state != NativePortAudioVoiceState::Completed)
                continue;
            const auto voice_frames = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(frames, available_frames(voice)));
            if (voice_frames == 0u) continue;
            const auto left_gain =
                static_cast<double>(voice.config.gain) *
                (voice.config.pan <= 0.0f
                     ? 1.0
                     : 1.0 - static_cast<double>(voice.config.pan));
            const auto right_gain =
                static_cast<double>(voice.config.gain) *
                (voice.config.pan >= 0.0f
                     ? 1.0
                     : 1.0 + static_cast<double>(voice.config.pan));
            for (std::uint32_t frame = 0u; frame < voice_frames; ++frame) {
                const auto source = (voice.read_frame + frame) * 2u;
                const auto destination = static_cast<std::size_t>(frame) * 2u;
                mix_accumulation_[destination] +=
                    static_cast<double>(voice.pcm[source]) * left_gain;
                mix_accumulation_[destination + 1u] +=
                    static_cast<double>(voice.pcm[source + 1u]) * right_gain;
            }
            voice.read_frame += voice_frames;
            saturating_add(voice.mixed_output_frames, voice_frames);
            pending_contributions_.push_back({&voice, voice_frames});
            compact_pcm(voice);
            if (voice.source == NativePortAudioVoiceSource::PcmFeed &&
                voice.feed_finished && available_frames(voice) == 0u)
                voice.state = NativePortAudioVoiceState::Completed;
        }

        if (!native_audio_signal_diagnostics_enabled()) {
            for (std::size_t sample = 0u; sample < sample_count; ++sample) {
                const auto rounded = std::llround(mix_accumulation_[sample]);
                const auto clamped = std::clamp<std::int64_t>(
                    rounded,
                    std::numeric_limits<std::int16_t>::min(),
                    std::numeric_limits<std::int16_t>::max());
                mixed_samples_[sample] = static_cast<std::int16_t>(clamped);
            }
        } else {
            pending_nonzero_samples_ = 0u;
            pending_clipped_samples_ = 0u;
            pending_peak_sample_ = 0u;
            for (std::size_t sample = 0u; sample < sample_count; ++sample) {
                const auto rounded = std::llround(mix_accumulation_[sample]);
                const auto clamped = std::clamp<std::int64_t>(
                    rounded,
                    std::numeric_limits<std::int16_t>::min(),
                    std::numeric_limits<std::int16_t>::max());
                const auto mixed = static_cast<std::int16_t>(clamped);
                mixed_samples_[sample] = mixed;
                pending_nonzero_samples_ += mixed != 0 ? 1u : 0u;
                pending_clipped_samples_ += rounded != clamped ? 1u : 0u;
                const auto signed_sample = static_cast<std::int32_t>(mixed);
                const auto magnitude = static_cast<std::uint32_t>(
                    signed_sample < 0 ? -signed_sample : signed_sample);
                pending_peak_sample_ =
                    std::max(pending_peak_sample_, magnitude);
            }
        }
        saturating_add(mixed_output_frames_, frames);
        return frames;
    }

    const NativePortCodecProvider& codec_provider_;
    NativePortAudioEngineConfig config_;
    std::thread::id owner_thread_;
    std::unique_ptr<NativePortAudioStream> output_;
    std::vector<Slot> slots_;
    std::vector<double> mix_accumulation_;
    std::vector<std::int16_t> mixed_samples_;
    std::vector<PendingContribution> pending_contributions_;
    std::uint64_t created_voices_ = 0u;
    std::uint64_t released_voices_ = 0u;
    std::uint64_t decoder_reads_ = 0u;
    std::uint64_t decoded_source_frames_ = 0u;
    std::uint64_t submitted_feed_frames_ = 0u;
    std::uint64_t mixed_output_frames_ = 0u;
    std::uint64_t submitted_output_frames_ = 0u;
    std::uint64_t submitted_nonzero_samples_ = 0u;
    std::uint64_t submitted_clipped_samples_ = 0u;
    std::uint64_t pending_nonzero_samples_ = 0u;
    std::uint64_t pending_clipped_samples_ = 0u;
    std::uint32_t submitted_peak_sample_ = 0u;
    std::uint32_t pending_peak_sample_ = 0u;
    bool output_paused_ = false;
};

class NativePortAudioEngine::Impl final {
  public:
    Impl(NativePortPlatformServices& platform,
         const NativePortCodecProvider& codec_provider,
         const NativePortAudioEngineConfig& config)
        : platform_(platform), codec_provider_(codec_provider), config_(config),
          owner_thread_(std::this_thread::get_id()),
          domain_(acquire_native_port_audio_execution_domain(
              NativePortAudioExecutionDomainConfig{
                  normalized_queue_config(config.command_queue)})) {
        validate_engine_config(config_);
        if (config_.telemetry != nullptr) {
            if (!domain_->bind_telemetry(config_.telemetry))
                fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                                  0u,
                                  "telemetry-bind");
            telemetry_bound_ = true;
        }
        const auto registered = domain_->register_target(
            NativePortAudioExecutionDomainTarget::AudioEngine,
            this,
            &Impl::execute_worker_command,
            &Impl::cleanup_worker_state);
        if (!registered.has_value()) {
            release_telemetry();
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u,
                              "engine-register");
        }
        handle_ = *registered;
        const auto result = domain_->dispatch_sync(
            handle_, static_cast<std::uint16_t>(AudioEngineOpcode::Construct),
            {}, 0u);
        if (!result.completed()) {
            // Construct can fail after Core acquired worker-only codec/output
            // state.  Keep the cleanup armed and synchronously join the one
            // consumer before this facade storage is unwound.
            domain_->shutdown();
            handle_ = {};
            release_telemetry();
            fail_audio_engine(NativePortAudioEngineFailure::WorkerFailure,
                              result.ack.error_code,
                              "engine-construct");
        }
    }

    ~Impl() {
        if (domain_ == nullptr || !handle_.valid()) return;
        if (sound_bank_handle_.valid()) {
            // Destruction order was violated while a dependent SoundBank is
            // still registered.  Only the consumer can tear both Cores down
            // safely; cleanup_registered_targets processes dependencies
            // before the engine and retires every slot exactly once.
            domain_->shutdown();
            sound_bank_handle_ = {};
            sound_bank_target_ = nullptr;
            handle_ = {};
            release_telemetry();
            domain_.reset();
            return;
        }
        const auto result = domain_->dispatch_sync(
            handle_, static_cast<std::uint16_t>(AudioEngineOpcode::Destroy),
            {}, current_frame_index_noexcept());
        if (result.completed()) {
            if (!domain_->unregister_target(handle_, this)) domain_->shutdown();
        } else {
            // Never destroy or leak worker-owned codec/mixer/endpoint state
            // on the simulation thread.  Terminal shutdown drains/cancels,
            // invokes cleanup_worker_state, and joins before returning.
            domain_->shutdown();
        }
        handle_ = {};
        release_telemetry();
        domain_.reset();
    }

    [[nodiscard]] NativePortAudioVoiceHandle create_voice(
        const NativePortContentFileBinding& binding,
        const NativePortAudioVoiceConfig& voice_config) {
        require_producer_thread();
        const auto content = materialize_audio_content(
            platform_, binding, config_.maximum_codec_source_bytes_per_voice);
        const auto logical_id = std::string(binding.logical_id);
        const auto path = binding.content_relative_path.generic_u8string();
        const auto identity = std::string(binding.byte_identity);
        std::uint64_t payload_size = sizeof(AudioCreateVoiceCommand);
        if (!checked_payload_sum(payload_size, logical_id.size()) ||
            !checked_payload_sum(payload_size, path.size()) ||
            !checked_payload_sum(payload_size, identity.size()) ||
            !checked_payload_sum(payload_size, content.size()))
            fail_audio_engine(NativePortAudioEngineFailure::ResourceLimit,
                              0u,
                              "voice-command-size");
        AudioCreateVoiceCommand header;
        header.config = voice_config;
        header.source_offset = binding.source_offset;
        header.byte_size = binding.byte_size;
        header.logical_id_size = static_cast<std::uint32_t>(logical_id.size());
        header.path_size = static_cast<std::uint32_t>(path.size());
        header.byte_identity_size =
            static_cast<std::uint32_t>(identity.size());
        const std::array parts{
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(&header),
                static_cast<std::uint32_t>(sizeof(header))},
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(logical_id.data()),
                static_cast<std::uint32_t>(logical_id.size())},
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(path.data()),
                static_cast<std::uint32_t>(path.size())},
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(identity.data()),
                static_cast<std::uint32_t>(identity.size())},
            NativePortAudioExecutionDomainPayloadPart{
                content.data(), static_cast<std::uint32_t>(content.size())}};
        return read_ack_value<NativePortAudioVoiceHandle>(
            dispatch_sync_scatter(AudioEngineOpcode::CreateVoice, parts,
                                  NativePortAudioExecutionDomainStage::AudioDecode),
            "create-voice");
    }

    [[nodiscard]] NativePortAudioVoiceHandle create_pcm_feed(
        const NativePortAudioVoiceConfig& voice_config) {
        const AudioCreatePcmFeedCommand command{voice_config};
        return read_ack_value<NativePortAudioVoiceHandle>(
            dispatch_sync(AudioEngineOpcode::CreatePcmFeed,
                          object_bytes(command)),
            "create-feed");
    }

    [[nodiscard]] bool submit_pcm_s16(
        const NativePortAudioVoiceHandle voice,
        const std::span<const std::int16_t> samples) {
        require_producer_thread();
        if (on_audio_thread())
            return core_->submit_pcm_s16(voice, samples);
        if (samples.size() > std::numeric_limits<std::uint32_t>::max())
            fail_audio_engine(NativePortAudioEngineFailure::ResourceLimit,
                              0u,
                              "feed-command-size");
        std::uint64_t payload_size = sizeof(AudioSubmitPcmCommand);
        if (!checked_payload_sum(payload_size, samples.size_bytes()))
            fail_audio_engine(NativePortAudioEngineFailure::ResourceLimit,
                              0u,
                              "feed-command-size");
        const AudioSubmitPcmCommand command{
            voice, static_cast<std::uint32_t>(samples.size())};
        const std::array parts{
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(&command),
                static_cast<std::uint32_t>(sizeof(command))},
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(samples.data()),
                static_cast<std::uint32_t>(samples.size_bytes())}};
        const auto ack = dispatch_sync_scatter(
            AudioEngineOpcode::SubmitPcm, parts,
            NativePortAudioExecutionDomainStage::None);
        return read_ack_value<std::uint8_t>(
                   ack,
                   "submit-feed") != 0u;
    }

    void handle_command(const AudioEngineOpcode opcode,
                        const NativePortAudioVoiceHandle voice) {
        const AudioHandleCommand command{voice};
        switch (opcode) {
        case AudioEngineOpcode::FinishPcmFeed:
        case AudioEngineOpcode::Play:
        case AudioEngineOpcode::Pause:
        case AudioEngineOpcode::Resume:
        case AudioEngineOpcode::Stop:
            dispatch_async(opcode, object_bytes(command));
            return;
        case AudioEngineOpcode::Release:
            // Release invalidates the generation-bound handle and therefore
            // remains an acknowledged lifecycle boundary.
            dispatch_sync_void(opcode, object_bytes(command),
                               "voice-release");
            return;
        default:
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u, "voice-control-opcode");
        }
    }

    void set_gain_pan(const NativePortAudioVoiceHandle voice,
                      const float gain,
        const float pan) {
        const AudioGainPanCommand command{voice, gain, pan};
        dispatch_async(AudioEngineOpcode::SetGainPan, object_bytes(command));
    }

    void set_output_paused(const bool paused) {
        const AudioPauseOutputCommand command{
            static_cast<std::uint8_t>(paused ? 1u : 0u), {}};
        dispatch_async(AudioEngineOpcode::SetOutputPaused,
                       object_bytes(command));
    }

    void stop_all() {
        dispatch_async(AudioEngineOpcode::StopAll, {});
    }

    void pump(const bool refresh_playback_position) {
        dispatch_async(refresh_playback_position ? AudioEngineOpcode::Pump
                                                 : AudioEngineOpcode::PumpCached,
                       {},
                       NativePortAudioExecutionDomainStage::AudioDecodeAndMix);
    }

    [[nodiscard]] NativePortAudioVoiceSnapshot voice_snapshot(
        const NativePortAudioVoiceHandle voice) const {
        const AudioHandleCommand command{voice};
        return read_ack_value<NativePortAudioVoiceSnapshot>(
            dispatch_sync(AudioEngineOpcode::VoiceSnapshot,
                          object_bytes(command)),
            "voice-snapshot");
    }

    [[nodiscard]] NativePortAudioEngineSnapshot snapshot() const {
        auto result = read_ack_value<NativePortAudioEngineSnapshot>(
            dispatch_sync(AudioEngineOpcode::Snapshot,
                          {}),
            "snapshot");
        result.command_queue = domain_->snapshot().queue;
        return result;
    }

    void bind_command_stamp(const NativePortAudioCommandStamp stamp) {
        require_producer_thread();
        bound_stamp_ = stamp;
    }

    [[nodiscard]] NativePortAudioCommandQueueSnapshot queue_snapshot()
        const noexcept {
        return domain_ != nullptr ? domain_->snapshot().queue
                                  : NativePortAudioCommandQueueSnapshot{};
    }

    void bind_sound_bank_target(void* const target,
                                const WorkerTargetExecutor executor,
                                const WorkerTargetCleanup cleanup) {
        require_producer_thread();
        if (target == nullptr || executor == nullptr || cleanup == nullptr ||
            sound_bank_handle_.valid())
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u,
                              "sound-bank-bind");
        const auto registered = domain_->register_target(
            NativePortAudioExecutionDomainTarget::SoundBank,
            target,
            executor,
            cleanup);
        if (!registered.has_value())
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u,
                              "sound-bank-register");
        sound_bank_handle_ = *registered;
        sound_bank_target_ = target;
    }

    void unbind_sound_bank_target(
        void* const target, const bool destroy_acknowledged) noexcept {
        if (target == nullptr || target != sound_bank_target_ ||
            !sound_bank_handle_.valid())
            return;
        if (destroy_acknowledged) {
            if (!domain_->unregister_target(sound_bank_handle_, target))
                domain_->shutdown();
        } else {
            // Preserve the cleanup registration.  The consumer owns the
            // terminal Core destruction and slot retirement.
            domain_->shutdown();
        }
        sound_bank_handle_ = {};
        sound_bank_target_ = nullptr;
    }

    [[nodiscard]] NativePortAudioCommandAck dispatch_sound_bank_sync(
        const std::uint16_t opcode,
        const std::span<const std::byte> payload) const {
        require_producer_thread();
        if (!sound_bank_handle_.valid())
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u,
                              "sound-bank-unbound");
        return checked_domain_ack(
            domain_->dispatch_sync(sound_bank_handle_, opcode, payload,
                                   current_frame_index()),
            "sound-bank-command");
    }

    void dispatch_sound_bank_async(
        const std::uint16_t opcode,
        const std::span<const std::byte> payload) const {
        require_producer_thread();
        if (!sound_bank_handle_.valid())
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u,
                              "sound-bank-unbound");
        require_domain_accepted(
            domain_->dispatch_async(
                sound_bank_handle_, opcode, payload, current_frame_index(),
                0u,
                NativePortAudioExecutionDomainStage::AudioDecodeAndMix),
            "sound-bank-async");
    }

    [[nodiscard]] bool on_audio_thread() const noexcept {
        return domain_ != nullptr && domain_->on_audio_thread();
    }

  private:
    void release_telemetry() noexcept {
        if (!telemetry_bound_ || domain_ == nullptr ||
            config_.telemetry == nullptr)
            return;
        if (!domain_->unbind_telemetry(config_.telemetry))
            domain_->shutdown();
        telemetry_bound_ = false;
    }

    [[nodiscard]] static NativePortAudioCommandQueueConfig
    normalized_queue_config(NativePortAudioCommandQueueConfig config) {
        if (!config.enabled)
            fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                              0u,
                              "command-queue-disabled");
        if (native_port_audio_serial_reference_requested())
            config.mode = NativePortAudioCommandQueueMode::SerialReference;
        return config;
    }

    void require_producer_thread() const {
        if (!on_audio_thread() && std::this_thread::get_id() != owner_thread_)
            fail_audio_engine(NativePortAudioEngineFailure::ThreadViolation,
                              0u,
                              "producer-thread");
    }

    [[nodiscard]] std::uint64_t current_frame_index() const {
        require_producer_thread();
        // Nested worker suboperations inherit the outer domain command. They
        // must not call a simulation-owned stamp callback or allocate a new
        // sequence; dispatch_inline ignores this evidence value.
        if (on_audio_thread()) return last_frame_index_;
        auto source = bound_stamp_;
        if (config_.command_stamp_source != nullptr)
            source = config_.command_stamp_source(config_.command_stamp_user);
        if (source.frame_index < last_frame_index_)
            fail_audio_engine(NativePortAudioEngineFailure::CommandQueue,
                              0u,
                              "frame-regression");
        auto frame_index = source.frame_index;
        if (const auto domain_frame = domain_->last_frame_index_nonblocking();
            domain_frame.has_value())
            frame_index = std::max(frame_index, *domain_frame);
        last_frame_index_ = frame_index;
        return frame_index;
    }

    [[nodiscard]] std::uint64_t current_frame_index_noexcept() const noexcept {
        return last_frame_index_;
    }

    [[nodiscard]] static NativePortAudioExecutionDomainStage engine_stage(
        const AudioEngineOpcode opcode) noexcept {
        switch (opcode) {
        case AudioEngineOpcode::CreateVoice:
            return NativePortAudioExecutionDomainStage::AudioDecode;
        case AudioEngineOpcode::Pump:
        case AudioEngineOpcode::PumpCached:
            return NativePortAudioExecutionDomainStage::AudioDecodeAndMix;
        default:
            return NativePortAudioExecutionDomainStage::None;
        }
    }

    [[nodiscard]] static NativePortAudioCommandAck checked_domain_ack(
        const NativePortAudioExecutionDomainDispatchResult& result,
        const char* const operation) {
        if (result.has_ack) return result.ack;
        const auto error_code = result.has_target_failure
                                    ? result.target_failure_error_code
                                    : static_cast<std::uint32_t>(result.failure);
        fail_audio_engine(
            result.failure ==
                    NativePortAudioExecutionDomainFailure::ProducerThreadViolation
                ? NativePortAudioEngineFailure::ThreadViolation
                : NativePortAudioEngineFailure::CommandQueue,
            error_code, operation);
    }

    static void require_domain_accepted(
        const NativePortAudioExecutionDomainDispatchResult& result,
        const char* const operation) {
        if (result.accepted()) return;
        const auto error_code = result.has_target_failure
                                    ? result.target_failure_error_code
                                    : static_cast<std::uint32_t>(result.failure);
        fail_audio_engine(
            result.failure ==
                    NativePortAudioExecutionDomainFailure::ProducerThreadViolation
                ? NativePortAudioEngineFailure::ThreadViolation
                : NativePortAudioEngineFailure::CommandQueue,
            error_code, operation);
    }

    [[nodiscard]] NativePortAudioCommandAck dispatch_sync(
        const AudioEngineOpcode opcode,
        const std::span<const std::byte> payload,
        const NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) const {
        require_producer_thread();
        return checked_domain_ack(
            domain_->dispatch_sync(
                handle_, static_cast<std::uint16_t>(opcode), payload,
                current_frame_index(), 0u,
                stage == NativePortAudioExecutionDomainStage::None
                    ? engine_stage(opcode)
                    : stage),
            "engine-command");
    }

    [[nodiscard]] NativePortAudioCommandAck dispatch_sync_scatter(
        const AudioEngineOpcode opcode,
        const std::span<const NativePortAudioExecutionDomainPayloadPart> parts,
        const NativePortAudioExecutionDomainStage stage) const {
        require_producer_thread();
        return checked_domain_ack(
            domain_->dispatch_sync_scatter(
                handle_, static_cast<std::uint16_t>(opcode), parts,
                current_frame_index(), 0u, stage),
            "engine-scatter-command");
    }

    void dispatch_sync_void(
        const AudioEngineOpcode opcode,
        const std::span<const std::byte> payload,
        const char* const operation) const {
        const auto ack = dispatch_sync(opcode, payload);
        require_ack_success(ack, operation);
        if (ack.result_size != 0u)
            fail_audio_engine(NativePortAudioEngineFailure::WorkerFailure,
                              0u, operation);
    }

    void dispatch_async(
        const AudioEngineOpcode opcode,
        const std::span<const std::byte> payload,
        const NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) const {
        require_producer_thread();
        require_domain_accepted(
            domain_->dispatch_async(
                handle_, static_cast<std::uint16_t>(opcode), payload,
                current_frame_index(), 0u,
                stage == NativePortAudioExecutionDomainStage::None
                    ? engine_stage(opcode)
                    : stage),
            "engine-async-command");
    }

    template <typename Value>
    [[nodiscard]] static Value read_ack_value(
        const NativePortAudioCommandAck& ack,
        const char* operation) {
        require_ack_success(ack, operation);
        if (ack.result_size != sizeof(Value))
            fail_audio_engine(NativePortAudioEngineFailure::WorkerFailure,
                              0u,
                              operation);
        Value result{};
        std::memcpy(&result, ack.bytes.data(), sizeof(Value));
        return result;
    }

    static void require_ack_success(const NativePortAudioCommandAck& ack,
                                    const char* operation) {
        if (ack.status == NativePortAudioCommandAckStatus::Completed) return;
        AudioEngineWireError error;
        if (ack.result_size == sizeof(error)) {
            std::memcpy(&error, ack.bytes.data(), sizeof(error));
            throw NativePortAudioEngineError(error.failure,
                                             error.provider_error_code,
                                             operation);
        }
        fail_audio_engine(NativePortAudioEngineFailure::WorkerFailure,
                          ack.error_code,
                          operation);
    }

    static void execute_worker_command(
        void* const target,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        NativePortAudioCommandAckResult& result) noexcept {
        result = {};
        auto* const self = static_cast<Impl*>(target);
        if (self == nullptr) {
            write_engine_error(result,
                               NativePortAudioEngineFailure::WorkerFailure,
                               0u);
            return;
        }
        try {
            self->execute_engine(static_cast<AudioEngineOpcode>(opcode),
                                 payload,
                                 result);
        } catch (const NativePortAudioEngineError& error) {
            write_engine_error(result,
                               error.failure(),
                               error.provider_error_code());
        } catch (...) {
            write_engine_error(result,
                               NativePortAudioEngineFailure::WorkerFailure,
                               0u);
        }
    }

    static void cleanup_worker_state(void* const target) noexcept {
        auto& self = *static_cast<Impl*>(target);
        self.core_.reset();
    }

    void execute_engine(const AudioEngineOpcode opcode,
                        const std::span<const std::byte> payload,
                        NativePortAudioCommandAckResult& result) const {
        switch (opcode) {
        case AudioEngineOpcode::Construct:
            if (!payload.empty() || core_ != nullptr)
                fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                                  0u,
                                  "engine-construct");
            core_ = std::make_unique<Core>(codec_provider_, config_);
            return;
        case AudioEngineOpcode::Destroy:
            if (!payload.empty())
                fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                                  0u,
                                  "engine-destroy");
            core_.reset();
            return;
        default:
            break;
        }
        if (core_ == nullptr)
            fail_audio_engine(NativePortAudioEngineFailure::WorkerFailure,
                              0u,
                              "worker-core");
        switch (opcode) {
        case AudioEngineOpcode::CreateVoice:
            execute_create_voice(payload, result);
            return;
        case AudioEngineOpcode::CreatePcmFeed: {
            AudioCreatePcmFeedCommand command;
            if (!read_object(payload, command))
                return invalid_payload(result);
            write_ack_value(result, core_->create_pcm_feed(command.config));
            return;
        }
        case AudioEngineOpcode::SubmitPcm:
            execute_submit_pcm(payload, result);
            return;
        case AudioEngineOpcode::FinishPcmFeed:
        case AudioEngineOpcode::Play:
        case AudioEngineOpcode::Pause:
        case AudioEngineOpcode::Resume:
        case AudioEngineOpcode::Stop:
        case AudioEngineOpcode::Release: {
            AudioHandleCommand command;
            if (!read_object(payload, command))
                return invalid_payload(result);
            switch (opcode) {
            case AudioEngineOpcode::FinishPcmFeed:
                core_->finish_pcm_feed(command.voice);
                break;
            case AudioEngineOpcode::Play:
                core_->play(command.voice);
                break;
            case AudioEngineOpcode::Pause:
                core_->pause(command.voice);
                break;
            case AudioEngineOpcode::Resume:
                core_->resume(command.voice);
                break;
            case AudioEngineOpcode::Stop:
                core_->stop(command.voice);
                break;
            case AudioEngineOpcode::Release:
                core_->release(command.voice);
                break;
            default:
                break;
            }
            return;
        }
        case AudioEngineOpcode::SetGainPan: {
            AudioGainPanCommand command;
            if (!read_object(payload, command))
                return invalid_payload(result);
            core_->set_gain_pan(command.voice, command.gain, command.pan);
            return;
        }
        case AudioEngineOpcode::SetOutputPaused: {
            AudioPauseOutputCommand command;
            if (!read_object(payload, command) || command.paused > 1u)
                return invalid_payload(result);
            core_->set_output_paused(command.paused != 0u);
            return;
        }
        case AudioEngineOpcode::StopAll:
            if (!payload.empty()) return invalid_payload(result);
            core_->stop_all();
            return;
        case AudioEngineOpcode::Pump:
        case AudioEngineOpcode::PumpCached:
            if (!payload.empty()) return invalid_payload(result);
            core_->pump(opcode == AudioEngineOpcode::Pump);
            return;
        case AudioEngineOpcode::VoiceSnapshot: {
            AudioHandleCommand command;
            if (!read_object(payload, command))
                return invalid_payload(result);
            write_ack_value(result, core_->voice_snapshot(command.voice));
            return;
        }
        case AudioEngineOpcode::Snapshot:
            if (!payload.empty()) return invalid_payload(result);
            write_ack_value(result, core_->snapshot());
            return;
        case AudioEngineOpcode::Construct:
        case AudioEngineOpcode::Destroy:
            break;
        }
        fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                          0u,
                          "engine-opcode");
    }

    void execute_create_voice(const std::span<const std::byte> payload,
                              NativePortAudioCommandAckResult& result) const {
        if (payload.size() < sizeof(AudioCreateVoiceCommand))
            return invalid_payload(result);
        AudioCreateVoiceCommand command;
        std::memcpy(&command, payload.data(), sizeof(command));
        std::uint64_t expected = sizeof(command);
        if (!checked_payload_sum(expected, command.logical_id_size) ||
            !checked_payload_sum(expected, command.path_size) ||
            !checked_payload_sum(expected, command.byte_identity_size) ||
            !checked_payload_sum(expected, command.byte_size) ||
            expected != payload.size())
            return invalid_payload(result);
        std::size_t cursor = sizeof(command);
        const std::string_view logical_id(
            reinterpret_cast<const char*>(payload.data() + cursor),
            command.logical_id_size);
        cursor += command.logical_id_size;
        const std::u8string path(
            reinterpret_cast<const char8_t*>(payload.data() + cursor),
            command.path_size);
        cursor += command.path_size;
        const std::string_view identity(
            reinterpret_cast<const char*>(payload.data() + cursor),
            command.byte_identity_size);
        cursor += command.byte_identity_size;
        std::vector<std::byte> content(command.byte_size);
        std::memcpy(content.data(), payload.data() + cursor, content.size());
        NativePortContentFileBinding binding{
            logical_id,
            std::filesystem::path(path),
            identity,
            command.source_offset,
            command.byte_size};
        write_ack_value(result,
                        core_->create_voice(binding,
                                            std::move(content),
                                            command.config));
    }

    void execute_submit_pcm(const std::span<const std::byte> payload,
                            NativePortAudioCommandAckResult& result) const {
        if (payload.size() < sizeof(AudioSubmitPcmCommand))
            return invalid_payload(result);
        AudioSubmitPcmCommand command;
        std::memcpy(&command, payload.data(), sizeof(command));
        const auto samples_bytes =
            static_cast<std::uint64_t>(command.sample_count) *
            sizeof(std::int16_t);
        if (samples_bytes > std::numeric_limits<std::size_t>::max() ||
            sizeof(command) + samples_bytes != payload.size())
            return invalid_payload(result);
        const auto* samples = reinterpret_cast<const std::int16_t*>(
            payload.data() + sizeof(command));
        const auto accepted = core_->submit_pcm_s16(
            command.voice,
            std::span<const std::int16_t>(samples, command.sample_count));
        write_ack_value(result,
                        static_cast<std::uint8_t>(accepted ? 1u : 0u));
    }

    static void invalid_payload(NativePortAudioCommandAckResult&) {
        fail_audio_engine(NativePortAudioEngineFailure::InvalidAudioBuffer,
                          0u,
                          "command-payload");
    }

    NativePortPlatformServices& platform_;
    const NativePortCodecProvider& codec_provider_;
    NativePortAudioEngineConfig config_;
    std::thread::id owner_thread_;
    std::shared_ptr<NativePortAudioExecutionDomain> domain_;
    NativePortAudioExecutionDomainTargetHandle handle_{};
    NativePortAudioExecutionDomainTargetHandle sound_bank_handle_{};
    mutable std::unique_ptr<Core> core_;
    mutable NativePortAudioCommandStamp bound_stamp_{};
    mutable std::uint64_t last_frame_index_ = 0u;
    void* sound_bank_target_ = nullptr;
    bool telemetry_bound_ = false;
};

NativePortAudioEngine::NativePortAudioEngine(
    NativePortPlatformServices& platform,
    const NativePortCodecProvider& codec_provider,
    const NativePortAudioEngineConfig& config)
    : impl_(std::make_unique<Impl>(platform, codec_provider, config)) {}

NativePortAudioEngine::~NativePortAudioEngine() = default;

NativePortAudioVoiceHandle NativePortAudioEngine::create_voice(
    const NativePortContentFileBinding& binding,
    const NativePortAudioVoiceConfig& config) {
    return impl_->create_voice(binding, config);
}

NativePortAudioVoiceHandle NativePortAudioEngine::create_pcm_feed(
    const NativePortAudioVoiceConfig& config) {
    return impl_->create_pcm_feed(config);
}

bool NativePortAudioEngine::submit_pcm_s16(
    const NativePortAudioVoiceHandle voice,
    const std::span<const std::int16_t> samples) {
    return impl_->submit_pcm_s16(voice, samples);
}

void NativePortAudioEngine::finish_pcm_feed(
    const NativePortAudioVoiceHandle voice) {
    impl_->handle_command(AudioEngineOpcode::FinishPcmFeed, voice);
}

void NativePortAudioEngine::play(const NativePortAudioVoiceHandle voice) {
    impl_->handle_command(AudioEngineOpcode::Play, voice);
}

void NativePortAudioEngine::pause(const NativePortAudioVoiceHandle voice) {
    impl_->handle_command(AudioEngineOpcode::Pause, voice);
}

void NativePortAudioEngine::resume(const NativePortAudioVoiceHandle voice) {
    impl_->handle_command(AudioEngineOpcode::Resume, voice);
}

void NativePortAudioEngine::stop(const NativePortAudioVoiceHandle voice) {
    impl_->handle_command(AudioEngineOpcode::Stop, voice);
}

void NativePortAudioEngine::release(const NativePortAudioVoiceHandle voice) {
    impl_->handle_command(AudioEngineOpcode::Release, voice);
}

void NativePortAudioEngine::set_gain_pan(
    const NativePortAudioVoiceHandle voice,
    const float gain,
    const float pan) {
    impl_->set_gain_pan(voice, gain, pan);
}

void NativePortAudioEngine::set_output_paused(const bool paused) {
    impl_->set_output_paused(paused);
}

void NativePortAudioEngine::stop_all() { impl_->stop_all(); }

void NativePortAudioEngine::pump() { impl_->pump(true); }

void NativePortAudioEngine::pump_with_cached_playback_position() {
    impl_->pump(false);
}

void NativePortAudioEngine::bind_command_stamp(
    const NativePortAudioCommandStamp stamp) {
    impl_->bind_command_stamp(stamp);
}

NativePortAudioCommandQueueSnapshot
NativePortAudioEngine::command_queue_snapshot() const {
    return impl_->queue_snapshot();
}

NativePortAudioVoiceSnapshot NativePortAudioEngine::voice_snapshot(
    const NativePortAudioVoiceHandle voice) const {
    return impl_->voice_snapshot(voice);
}

NativePortAudioEngineSnapshot NativePortAudioEngine::snapshot() const {
    return impl_->snapshot();
}

void NativePortAudioEngine::bind_sound_bank_target(
    void* const target,
    const WorkerTargetExecutor executor,
    const WorkerTargetCleanup cleanup) {
    impl_->bind_sound_bank_target(target, executor, cleanup);
}

void NativePortAudioEngine::unbind_sound_bank_target(
    void* const target, const bool destroy_acknowledged) noexcept {
    impl_->unbind_sound_bank_target(target, destroy_acknowledged);
}

NativePortAudioCommandAck NativePortAudioEngine::dispatch_sound_bank_sync(
    const std::uint16_t opcode,
    const std::span<const std::byte> payload) const {
    return impl_->dispatch_sound_bank_sync(opcode, payload);
}

void NativePortAudioEngine::dispatch_sound_bank_async(
    const std::uint16_t opcode,
    const std::span<const std::byte> payload) const {
    impl_->dispatch_sound_bank_async(opcode, payload);
}

bool NativePortAudioEngine::on_audio_thread() const noexcept {
    return impl_->on_audio_thread();
}

} // namespace katana::runtime
