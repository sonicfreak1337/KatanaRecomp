#include "katana/runtime/native_port_audio_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

constexpr std::uint32_t maximum_native_audio_voices = 4'096u;
constexpr std::uint32_t maximum_mix_block_frames = 16'384u;
constexpr std::uint32_t maximum_decoder_reads_per_pump = 1'048'576u;

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
            maximum_decoder_reads_per_pump)
        fail_audio_engine(NativePortAudioEngineFailure::InvalidConfig,
                          0u,
                          "config");
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

class NativePortAudioEngine::Impl final {
  private:
    struct Voice;

  public:
    Impl(NativePortPlatformServices& platform,
         const NativePortCodecProvider& codec_provider,
         const NativePortAudioEngineConfig& config)
        : platform_(platform), codec_provider_(codec_provider), config_(config),
          owner_thread_(std::this_thread::get_id()) {
        validate_engine_config(config_);
        if (!valid_native_port_codec_provider(codec_provider_))
            fail_audio_engine(NativePortAudioEngineFailure::InvalidProvider,
                              0u,
                              "provider");
        try {
            output_ = std::make_unique<NativePortAudioStream>(
                NativePortAudioConfig{config_.output_format,
                                      config_.maximum_output_queue_frames});
        } catch (...) {
            fail_audio_engine(NativePortAudioEngineFailure::HostAudio,
                              0u,
                              "output-open");
        }
        mix_accumulation_.resize(
            static_cast<std::size_t>(config_.mix_block_frames) * 2u);
        mixed_samples_.resize(
            static_cast<std::size_t>(config_.mix_block_frames) * 2u);
    }

    ~Impl() {
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
        try {
            voice->file = platform_.open_content_file(voice->binding());
        } catch (const NativePortPlatformError& error) {
            fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                              error.platform_error_code(),
                              "content-open");
        } catch (...) {
            fail_audio_engine(NativePortAudioEngineFailure::ContentOpen,
                              0u,
                              "content-open");
        }
        open_decoder(*voice, voice->config.start_frame);

        return insert_voice(std::move(voice));
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

    [[nodiscard]] NativePortAudioVoiceHandle insert_voice(
        std::unique_ptr<Voice> voice) {
        std::size_t slot_index = slots_.size();
        for (std::size_t index = 0u; index < slots_.size(); ++index) {
            if (!slots_[index].voice) {
                slot_index = index;
                break;
            }
        }
        if (slot_index == slots_.size()) {
            if (slots_.size() == config_.maximum_voices) {
                close_decoder(*voice);
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

    void pump() {
        require_owner_thread();
        try {
            output_->poll();
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

        std::uint32_t decoder_reads_remaining =
            config_.maximum_decoder_reads_per_pump;
        while (output_snapshot.queued_frames <
               config_.target_output_queue_frames) {
            const auto queue_room =
                config_.target_output_queue_frames -
                static_cast<std::uint32_t>(output_snapshot.queued_frames);
            const auto requested_frames =
                std::min(config_.mix_block_frames, queue_room);
            const auto mixed_frames =
                mix_block(requested_frames, decoder_reads_remaining);
            if (mixed_frames == 0u) break;
            const auto sample_count =
                static_cast<std::size_t>(mixed_frames) * 2u;
            bool submitted = false;
            try {
                submitted = output_->submit_pcm_s16(
                    std::span<const std::int16_t>(mixed_samples_.data(),
                                                  sample_count));
            } catch (...) {
                fail_audio_engine(NativePortAudioEngineFailure::HostAudio,
                                  0u,
                                  "output-submit");
            }
            if (!submitted) break;
            saturating_add(submitted_output_frames_, mixed_frames);
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
    struct Voice final {
        Impl* owner = nullptr;
        NativePortAudioVoiceSource source =
            NativePortAudioVoiceSource::Codec;
        std::string logical_id;
        std::filesystem::path content_relative_path;
        std::string byte_identity;
        std::uint64_t source_offset = 0u;
        std::uint64_t byte_size = 0u;
        NativePortAudioVoiceConfig config;
        std::unique_ptr<NativePortReadOnlyFile> file;
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
        std::uint64_t loop_count = 0u;
        std::uint64_t frames_since_restart = 0u;
        std::uint32_t consecutive_empty_loops = 0u;
        std::vector<std::int16_t> pcm;
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
        if (voice.file == nullptr || destination == nullptr ||
            offset > voice.file->byte_size() ||
            byte_count > voice.file->byte_size() - offset ||
            byte_count > std::numeric_limits<std::size_t>::max())
            return 0u;
        try {
            voice.file->read_at(
                offset,
                std::span<std::byte>(static_cast<std::byte*>(destination),
                                     static_cast<std::size_t>(byte_count)));
            return 1u;
        } catch (...) {
            return 0u;
        }
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
        request.source = {&voice, voice.file->byte_size(), &Impl::provider_read_at};
        request.requested_audio_sample_rate = config_.output_format.sample_rate;
        request.maximum_audio_queue_frames =
            config_.maximum_buffered_frames_per_voice;
        request.maximum_video_queue_frames = 1u;
        request.maximum_video_frame_bytes = 1u;
        request.maximum_video_queue_bytes = 1u;
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
            compact_pcm(voice);
            if (voice.source == NativePortAudioVoiceSource::PcmFeed &&
                voice.feed_finished && available_frames(voice) == 0u)
                voice.state = NativePortAudioVoiceState::Completed;
        }

        for (std::size_t sample = 0u; sample < sample_count; ++sample) {
            const auto rounded = std::llround(mix_accumulation_[sample]);
            const auto clamped = std::clamp<std::int64_t>(
                rounded,
                std::numeric_limits<std::int16_t>::min(),
                std::numeric_limits<std::int16_t>::max());
            mixed_samples_[sample] = static_cast<std::int16_t>(clamped);
        }
        saturating_add(mixed_output_frames_, frames);
        return frames;
    }

    NativePortPlatformServices& platform_;
    const NativePortCodecProvider& codec_provider_;
    NativePortAudioEngineConfig config_;
    std::thread::id owner_thread_;
    std::unique_ptr<NativePortAudioStream> output_;
    std::vector<Slot> slots_;
    std::vector<double> mix_accumulation_;
    std::vector<std::int16_t> mixed_samples_;
    std::uint64_t created_voices_ = 0u;
    std::uint64_t released_voices_ = 0u;
    std::uint64_t decoder_reads_ = 0u;
    std::uint64_t decoded_source_frames_ = 0u;
    std::uint64_t submitted_feed_frames_ = 0u;
    std::uint64_t mixed_output_frames_ = 0u;
    std::uint64_t submitted_output_frames_ = 0u;
    bool output_paused_ = false;
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
    impl_->finish_pcm_feed(voice);
}

void NativePortAudioEngine::play(const NativePortAudioVoiceHandle voice) {
    impl_->play(voice);
}

void NativePortAudioEngine::pause(const NativePortAudioVoiceHandle voice) {
    impl_->pause(voice);
}

void NativePortAudioEngine::resume(const NativePortAudioVoiceHandle voice) {
    impl_->resume(voice);
}

void NativePortAudioEngine::stop(const NativePortAudioVoiceHandle voice) {
    impl_->stop(voice);
}

void NativePortAudioEngine::release(const NativePortAudioVoiceHandle voice) {
    impl_->release(voice);
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

void NativePortAudioEngine::stop_all() {
    impl_->stop_all();
}

void NativePortAudioEngine::pump() {
    impl_->pump();
}

NativePortAudioVoiceSnapshot NativePortAudioEngine::voice_snapshot(
    const NativePortAudioVoiceHandle voice) const {
    return impl_->voice_snapshot(voice);
}

NativePortAudioEngineSnapshot NativePortAudioEngine::snapshot() const {
    return impl_->snapshot();
}

} // namespace katana::runtime
