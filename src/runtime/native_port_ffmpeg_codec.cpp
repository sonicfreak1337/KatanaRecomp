#include "katana/runtime/native_port_ffmpeg_codec.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace katana::runtime {
namespace {

constexpr AVRational nanosecond_time_base{1, 1'000'000'000};
constexpr int io_buffer_size = 64 * 1024;
constexpr std::uint32_t maximum_provider_audio_queue_frames = 192'000u * 60u;
constexpr std::uint32_t maximum_provider_video_queue_frames = 256u;
constexpr std::uint64_t maximum_provider_video_frame_bytes = 128u * 1024u * 1024u;
constexpr std::uint64_t maximum_provider_video_queue_bytes = 128u * 1024u * 1024u;
constexpr std::int64_t maximum_probe_bytes = 32ll * 1024ll * 1024ll;
constexpr std::int64_t maximum_analyze_duration = 10ll * AV_TIME_BASE;
constexpr unsigned int maximum_index_bytes = 16u * 1024u * 1024u;
constexpr std::uint64_t maximum_sofdec_signature_probe_bytes = 4u * 1024u * 1024u;
constexpr std::string_view sofdec_signature = "Sofdec";

[[nodiscard]] bool valid_component_abi(const unsigned version,
                                       const unsigned expected_major) noexcept {
    return (version >> 16u) == expected_major;
}

[[nodiscard]] bool valid_lgpl_component(const char* license,
                                        const char* configuration) noexcept {
    if (license == nullptr || configuration == nullptr) return false;
    const std::string_view license_view(license);
    const std::string_view configuration_view(configuration);
    return license_view.starts_with("LGPL") &&
           configuration_view.find("--enable-gpl") == std::string_view::npos &&
           configuration_view.find("--enable-nonfree") == std::string_view::npos;
}

struct OutputSample final {
    NativePortCodecSampleKind kind = NativePortCodecSampleKind::AudioEnd;
    std::uint64_t timestamp = 0u;
    std::uint64_t duration = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint32_t channels = 0u;
    std::vector<std::int16_t> audio;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t stride = 0u;
    std::vector<std::byte> pixels;
};

class FfmpegDecoder final {
  public:
    explicit FfmpegDecoder(const NativePortCodecOpenRequest& request)
        : source_(request.source), requested_audio_rate_(request.requested_audio_sample_rate),
          maximum_audio_queue_frames_(request.maximum_audio_queue_frames),
          maximum_video_queue_frames_(request.maximum_video_queue_frames),
          maximum_video_frame_bytes_(request.maximum_video_frame_bytes),
          maximum_video_queue_bytes_(request.maximum_video_queue_bytes) {}

    ~FfmpegDecoder() {
        if (packet_ != nullptr) av_packet_free(&packet_);
        if (frame_ != nullptr) av_frame_free(&frame_);
        if (audio_resampler_ != nullptr) swr_free(&audio_resampler_);
        if (video_scaler_ != nullptr) sws_freeContext(video_scaler_);
        if (audio_codec_ != nullptr) avcodec_free_context(&audio_codec_);
        if (video_codec_ != nullptr) avcodec_free_context(&video_codec_);
        if (format_ != nullptr) avformat_close_input(&format_);
        if (io_ != nullptr) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
    }

    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    [[nodiscard]] bool initialize() {
        if (!valid_component_abi(avformat_version(), LIBAVFORMAT_VERSION_MAJOR) ||
            !valid_component_abi(avcodec_version(), LIBAVCODEC_VERSION_MAJOR) ||
            !valid_component_abi(avutil_version(), LIBAVUTIL_VERSION_MAJOR) ||
            !valid_component_abi(swresample_version(), LIBSWRESAMPLE_VERSION_MAJOR) ||
            !valid_component_abi(swscale_version(), LIBSWSCALE_VERSION_MAJOR))
            return fail(NativePortCodecFailure::InvalidContract, 2);
        if (!valid_lgpl_component(avformat_license(), avformat_configuration()) ||
            !valid_lgpl_component(avcodec_license(), avcodec_configuration()) ||
            !valid_lgpl_component(avutil_license(), avutil_configuration()) ||
            !valid_lgpl_component(swresample_license(), swresample_configuration()) ||
            !valid_lgpl_component(swscale_license(), swscale_configuration()))
            return fail(NativePortCodecFailure::InvalidContract, 1);

        if (!detect_headerless_sofdec()) return false;
        if (inject_sofdec_signature_ &&
            source_.byte_size > static_cast<std::uint64_t>(INT64_MAX) - sofdec_signature.size())
            return fail(NativePortCodecFailure::InvalidContract, AVERROR(EOVERFLOW));

        auto* io_buffer = static_cast<unsigned char*>(av_malloc(io_buffer_size));
        if (io_buffer == nullptr) return fail(NativePortCodecFailure::ResourceExhausted, ENOMEM);
        io_ = avio_alloc_context(io_buffer,
                                 io_buffer_size,
                                 0,
                                 this,
                                 &FfmpegDecoder::read_packet,
                                 nullptr,
                                 &FfmpegDecoder::seek);
        if (io_ == nullptr) {
            av_free(io_buffer);
            return fail(NativePortCodecFailure::ResourceExhausted, ENOMEM);
        }
        io_->seekable = AVIO_SEEKABLE_NORMAL;
        format_ = avformat_alloc_context();
        if (format_ == nullptr) return fail(NativePortCodecFailure::ResourceExhausted, ENOMEM);
        format_->pb = io_;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;
        format_->max_streams = 8;
        format_->probesize = static_cast<std::int64_t>(
            std::min<std::uint64_t>(virtual_source_size(),
                                    static_cast<std::uint64_t>(maximum_probe_bytes)));
        format_->max_analyze_duration = maximum_analyze_duration;
        format_->max_index_size = maximum_index_bytes;
        format_->max_picture_buffer = static_cast<unsigned int>(
            std::min<std::uint64_t>(maximum_video_queue_bytes_,
                                    std::numeric_limits<unsigned int>::max()));
        auto result = avformat_open_input(&format_, nullptr, nullptr, nullptr);
        if (result < 0)
            return fail(result == AVERROR(ENOMEM)
                            ? NativePortCodecFailure::ResourceExhausted
                            : NativePortCodecFailure::UnsupportedContainer,
                        result);
        result = avformat_find_stream_info(format_, nullptr);
        if (result < 0) return fail(map_error(result), result);

        audio_stream_index_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        video_stream_index_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (audio_stream_index_ >= 0 && !open_codec(audio_stream_index_, audio_codec_))
            return false;
        if (video_stream_index_ >= 0 && !open_codec(video_stream_index_, video_codec_))
            return false;
        if (audio_stream_index_ < 0 && video_stream_index_ < 0)
            return fail(NativePortCodecFailure::UnsupportedContainer, AVERROR_STREAM_NOT_FOUND);
        // A missing optional stream is already terminal. Leaving its drained
        // bit false would make an otherwise valid audio-only or video-only
        // source end as an internal decoder failure after its real stream
        // drains.
        audio_drained_ = audio_codec_ == nullptr;
        video_drained_ = video_codec_ == nullptr;

        if (audio_codec_ != nullptr) {
            if (audio_codec_->sample_rate <= 0 || audio_codec_->ch_layout.nb_channels <= 0)
                return fail(NativePortCodecFailure::InvalidData, AVERROR_INVALIDDATA);
            audio_channels_ = audio_codec_->ch_layout.nb_channels == 1 ? 1u : 2u;
            AVChannelLayout output_layout{};
            av_channel_layout_default(&output_layout, static_cast<int>(audio_channels_));
            result = swr_alloc_set_opts2(&audio_resampler_,
                                         &output_layout,
                                         AV_SAMPLE_FMT_S16,
                                         static_cast<int>(requested_audio_rate_),
                                         &audio_codec_->ch_layout,
                                         audio_codec_->sample_fmt,
                                         audio_codec_->sample_rate,
                                         0,
                                         nullptr);
            av_channel_layout_uninit(&output_layout);
            if (result < 0 || audio_resampler_ == nullptr) return fail(map_error(result), result);
            result = swr_init(audio_resampler_);
            if (result < 0) return fail(map_error(result), result);
        }

        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (packet_ == nullptr || frame_ == nullptr)
            return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));

        start_time_nanoseconds_ = 0;
        if (format_->start_time != AV_NOPTS_VALUE)
            start_time_nanoseconds_ =
                av_rescale_q(format_->start_time, AV_TIME_BASE_Q, nanosecond_time_base);
        duration_nanoseconds_ =
            format_->duration > 0
                ? positive_ns(av_rescale_q(format_->duration, AV_TIME_BASE_Q, nanosecond_time_base))
                : 0u;
        return true;
    }

    void describe(NativePortCodecOpenResult& result) noexcept {
        result.decoder = this;
        result.duration_nanoseconds = duration_nanoseconds_;
        result.failure = NativePortCodecFailure::None;
        result.audio_sample_rate = audio_codec_ != nullptr ? requested_audio_rate_ : 0u;
        result.audio_channels = audio_codec_ != nullptr ? audio_channels_ : 0u;
        result.has_audio = audio_codec_ != nullptr ? 1u : 0u;
        result.has_video = video_codec_ != nullptr ? 1u : 0u;
    }

    void read(NativePortCodecReadResult& result) {
        current_ = {};
        if (failure_ != NativePortCodecFailure::None) {
            write_failure(result);
            return;
        }
        if (!fill_candidates()) {
            write_failure(result);
            return;
        }
        const bool audio_ready = !audio_queue_.empty();
        const bool video_ready = !video_queue_.empty();
        if (!audio_ready && !video_ready) {
            if (audio_drained_ && video_drained_) {
                result = {};
                result.status = NativePortCodecReadStatus::EndOfStream;
                result.failure = NativePortCodecFailure::None;
                return;
            }
            static_cast<void>(fail(NativePortCodecFailure::Internal, AVERROR_BUG));
            write_failure(result);
            return;
        }
        if (audio_ready &&
            (!video_ready || audio_queue_.front().timestamp <= video_queue_.front().timestamp)) {
            current_ = std::move(audio_queue_.front());
            audio_queue_.pop_front();
            queued_audio_frames_ -= current_.audio.size() / current_.channels;
        } else {
            current_ = std::move(video_queue_.front());
            video_queue_.pop_front();
            queued_video_bytes_ -= current_.pixels.size();
        }
        if (global_timestamp_initialized_ && current_.timestamp < last_global_timestamp_) {
            static_cast<void>(fail(NativePortCodecFailure::InvalidData, AVERROR_INVALIDDATA));
            write_failure(result);
            return;
        }
        global_timestamp_initialized_ = true;
        last_global_timestamp_ = current_.timestamp;
        result = {};
        result.status = NativePortCodecReadStatus::Sample;
        result.failure = NativePortCodecFailure::None;
        result.sample.kind = current_.kind;
        result.sample.timestamp_nanoseconds = current_.timestamp;
        result.sample.duration_nanoseconds = current_.duration;
        if (current_.kind == NativePortCodecSampleKind::Audio) {
            result.sample.audio_sample_rate = current_.sample_rate;
            result.sample.audio_channels = current_.channels;
            result.sample.audio_samples = current_.audio.data();
            result.sample.audio_sample_count = current_.audio.size();
        } else {
            result.sample.video_width = current_.width;
            result.sample.video_height = current_.height;
            result.sample.video_stride_bytes = current_.stride;
            result.sample.video_pixels = current_.pixels.data();
            result.sample.video_byte_count = current_.pixels.size();
        }
    }

    [[nodiscard]] NativePortCodecFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] std::uint32_t error_code() const noexcept {
        return error_code_;
    }

  private:
    [[nodiscard]] static std::uint16_t read_be16(const std::uint8_t* data) noexcept {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8u) |
                                          data[1]);
    }

    [[nodiscard]] static std::uint32_t read_be32(const std::uint8_t* data) noexcept {
        return (static_cast<std::uint32_t>(data[0]) << 24u) |
               (static_cast<std::uint32_t>(data[1]) << 16u) |
               (static_cast<std::uint32_t>(data[2]) << 8u) |
               static_cast<std::uint32_t>(data[3]);
    }

    [[nodiscard]] static bool valid_adx_header(const std::uint8_t* data,
                                               const std::size_t size) noexcept {
        if (data == nullptr || size < 24u || read_be16(data) != 0x8000u)
            return false;
        const auto copyright_offset = static_cast<std::uint32_t>(read_be16(data + 2u));
        const auto header_size = copyright_offset + 4u;
        if (copyright_offset < 8u || header_size > size ||
            copyright_offset < 2u ||
            std::memcmp(data + copyright_offset - 2u, "(c)CRI", 6u) != 0 || data[4] != 3u ||
            data[5] != 18u || data[6] != 4u)
            return false;
        const auto channels = static_cast<std::uint32_t>(data[7]);
        const auto sample_rate = read_be32(data + 8u);
        if (channels == 0u || channels > 8u || sample_rate < 8'000u || sample_rate > 192'000u)
            return false;
        return true;
    }

    [[nodiscard]] bool detect_headerless_sofdec() {
        const auto probe_size = static_cast<std::size_t>(std::min<std::uint64_t>(
            source_.byte_size, maximum_sofdec_signature_probe_bytes));
        if (probe_size < 32u) return true;
        std::vector<std::uint8_t> bytes(probe_size);
        if (source_.read_at(source_.user, 0u, bytes.data(), bytes.size()) == 0u)
            return fail(NativePortCodecFailure::ContentRead, AVERROR(EIO));
        constexpr std::uint8_t pack_header[] = {0x00u, 0x00u, 0x01u, 0xbau};
        if (!std::equal(std::begin(pack_header), std::end(pack_header), bytes.begin()) ||
            (bytes.size() >= sofdec_signature.size() &&
             std::memcmp(bytes.data(), sofdec_signature.data(), sofdec_signature.size()) == 0))
            return true;
        for (std::size_t offset = 0u; offset + 6u < bytes.size(); ++offset) {
            if (bytes[offset] != 0u || bytes[offset + 1u] != 0u || bytes[offset + 2u] != 1u ||
                bytes[offset + 3u] < 0xc0u || bytes[offset + 3u] > 0xdfu)
                continue;
            const auto pes_size = static_cast<std::size_t>(read_be16(bytes.data() + offset + 4u));
            if (pes_size == 0u || pes_size > bytes.size() - (offset + 6u)) continue;
            const auto packet_end = offset + 6u + pes_size;
            const auto search_begin = offset + 6u;
            const auto search_end = std::min(packet_end, search_begin + 128u);
            for (auto candidate = search_begin; candidate + 24u <= search_end; ++candidate) {
                if (valid_adx_header(bytes.data() + candidate, packet_end - candidate)) {
                    inject_sofdec_signature_ = true;
                    return true;
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t virtual_source_size() const noexcept {
        return source_.byte_size + (inject_sofdec_signature_ ? sofdec_signature.size() : 0u);
    }

    static int read_packet(void* opaque, std::uint8_t* buffer, const int requested) noexcept {
        auto& self = *static_cast<FfmpegDecoder*>(opaque);
        if (requested <= 0) return 0;
        const auto source_size = self.virtual_source_size();
        if (self.position_ >= source_size) return AVERROR_EOF;
        const auto count = std::min<std::uint64_t>(static_cast<std::uint64_t>(requested),
                                                   source_size - self.position_);
        auto copied = std::uint64_t{0u};
        if (self.inject_sofdec_signature_ && self.position_ < sofdec_signature.size()) {
            const auto prefix_count = std::min<std::uint64_t>(
                count, sofdec_signature.size() - self.position_);
            std::memcpy(buffer, sofdec_signature.data() + self.position_, prefix_count);
            copied += prefix_count;
        }
        if (copied < count) {
            const auto virtual_position = self.position_ + copied;
            const auto source_position =
                virtual_position - (self.inject_sofdec_signature_ ? sofdec_signature.size() : 0u);
            if (self.source_.read_at(self.source_.user,
                                     source_position,
                                     buffer + copied,
                                     count - copied) == 0u)
                return AVERROR(EIO);
        }
        self.position_ += count;
        return static_cast<int>(count);
    }

    static std::int64_t seek(void* opaque, const std::int64_t offset, const int origin) noexcept {
        auto& self = *static_cast<FfmpegDecoder*>(opaque);
        if ((origin & AVSEEK_SIZE) != 0) {
            if (self.virtual_source_size() > static_cast<std::uint64_t>(INT64_MAX))
                return AVERROR(EOVERFLOW);
            return static_cast<std::int64_t>(self.virtual_source_size());
        }
        const int whence = origin & ~AVSEEK_FORCE;
        std::int64_t base = 0;
        if (whence == SEEK_CUR)
            base = static_cast<std::int64_t>(self.position_);
        else if (whence == SEEK_END) {
            if (self.virtual_source_size() > static_cast<std::uint64_t>(INT64_MAX))
                return AVERROR(EOVERFLOW);
            base = static_cast<std::int64_t>(self.virtual_source_size());
        } else if (whence != SEEK_SET) {
            return AVERROR(EINVAL);
        }
        if ((offset < 0 && offset < -base) || (offset > 0 && offset > INT64_MAX - base))
            return AVERROR(EINVAL);
        const auto position = base + offset;
        if (position < 0 || static_cast<std::uint64_t>(position) > self.virtual_source_size())
            return AVERROR(EINVAL);
        self.position_ = static_cast<std::uint64_t>(position);
        return position;
    }

    [[nodiscard]] bool open_codec(const int stream_index, AVCodecContext*& context) {
        const auto* parameters = format_->streams[stream_index]->codecpar;
        const auto* decoder = avcodec_find_decoder(parameters->codec_id);
        if (decoder == nullptr)
            return fail(NativePortCodecFailure::UnsupportedCodec, AVERROR_DECODER_NOT_FOUND);
        context = avcodec_alloc_context3(decoder);
        if (context == nullptr)
            return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
        auto result = avcodec_parameters_to_context(context, parameters);
        if (result < 0) return fail(map_error(result), result);
        context->pkt_timebase = format_->streams[stream_index]->time_base;
        result = avcodec_open2(context, decoder, nullptr);
        if (result < 0) return fail(map_error(result), result);
        return true;
    }

    [[nodiscard]] bool fill_candidates() {
        for (std::size_t work = 0u; work < 65'536u; ++work) {
            const bool need_audio =
                audio_codec_ != nullptr && !audio_drained_ && audio_queue_.empty();
            const bool need_video =
                video_codec_ != nullptr && !video_drained_ && video_queue_.empty();
            if (!need_audio && !need_video) return true;
            bool progress = false;
            if (!demux_eof_) {
                const auto result = av_read_frame(format_, packet_);
                if (result == AVERROR_EOF) {
                    demux_eof_ = true;
                    progress = true;
                } else if (result < 0) {
                    return fail(map_error(result), result);
                } else {
                    progress = true;
                    if (packet_->stream_index == audio_stream_index_)
                        progress = decode_packet(audio_codec_, packet_, true);
                    else if (packet_->stream_index == video_stream_index_)
                        progress = decode_packet(video_codec_, packet_, false);
                    av_packet_unref(packet_);
                    if (!progress) return false;
                }
            } else {
                progress = flush_decoder(audio_codec_, true) || progress;
                if (failure_ != NativePortCodecFailure::None) return false;
                progress = flush_decoder(video_codec_, false) || progress;
                if (failure_ != NativePortCodecFailure::None) return false;
            }
            if (!progress) return fail(NativePortCodecFailure::Internal, AVERROR_BUG);
        }
        return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
    }

    [[nodiscard]] bool
    decode_packet(AVCodecContext* codec, const AVPacket* packet, const bool audio) {
        auto result = avcodec_send_packet(codec, packet);
        if (result == AVERROR(EAGAIN)) {
            if (!drain_frames(codec, audio)) return false;
            result = avcodec_send_packet(codec, packet);
        }
        if (result < 0) return fail(map_error(result), result);
        return drain_frames(codec, audio);
    }

    [[nodiscard]] bool drain_frames(AVCodecContext* codec, const bool audio) {
        for (;;) {
            const auto result = avcodec_receive_frame(codec, frame_);
            if (result == AVERROR(EAGAIN)) return true;
            if (result == AVERROR_EOF) {
                if (audio) {
                    audio_drained_ = true;
                    return flush_resampler();
                }
                video_drained_ = true;
                return true;
            }
            if (result < 0) return fail(map_error(result), result);
            const bool converted = audio ? convert_audio_frame() : convert_video_frame();
            av_frame_unref(frame_);
            if (!converted) return false;
        }
    }

    [[nodiscard]] bool flush_decoder(AVCodecContext* codec, const bool audio) {
        if (codec == nullptr || (audio ? audio_drained_ : video_drained_)) return false;
        auto& sent = audio ? audio_flush_sent_ : video_flush_sent_;
        if (!sent) {
            const auto result = avcodec_send_packet(codec, nullptr);
            if (result < 0 && result != AVERROR_EOF) return fail(map_error(result), result);
            sent = true;
        }
        const auto before_audio = audio_queue_.size();
        const auto before_video = video_queue_.size();
        if (!drain_frames(codec, audio)) return false;
        return (audio ? audio_queue_.size() != before_audio
                      : video_queue_.size() != before_video) ||
               (audio ? audio_drained_ : video_drained_);
    }

    [[nodiscard]] bool convert_audio_frame() {
        if (frame_->nb_samples <= 0) return true;
        const auto capacity = swr_get_out_samples(audio_resampler_, frame_->nb_samples);
        if (capacity < 0) return fail(map_error(capacity), capacity);
        const auto remaining_frames =
            maximum_audio_queue_frames_ -
            std::min<std::uint64_t>(queued_audio_frames_, maximum_audio_queue_frames_);
        if (static_cast<std::uint64_t>(capacity) > remaining_frames)
            return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
        OutputSample sample;
        sample.kind = NativePortCodecSampleKind::Audio;
        sample.sample_rate = requested_audio_rate_;
        sample.channels = audio_channels_;
        sample.audio.resize(static_cast<std::size_t>(capacity) * audio_channels_);
        auto* output = reinterpret_cast<std::uint8_t*>(sample.audio.data());
        const auto converted = swr_convert(audio_resampler_,
                                           &output,
                                           capacity,
                                           const_cast<const std::uint8_t**>(frame_->extended_data),
                                           frame_->nb_samples);
        if (converted < 0) return fail(map_error(converted), converted);
        sample.audio.resize(static_cast<std::size_t>(converted) * audio_channels_);
        if (sample.audio.empty()) return true;
        sample.timestamp = frame_timestamp(*frame_, audio_stream_index_, next_audio_timestamp_);
        sample.duration = samples_to_ns(converted);
        next_audio_timestamp_ = saturating_add(sample.timestamp, sample.duration);
        if (!validate_stream_timestamp(sample.timestamp, true)) return false;
        const auto frames = sample.audio.size() / audio_channels_;
        if (frames > maximum_audio_queue_frames_ -
                         std::min<std::uint64_t>(queued_audio_frames_, maximum_audio_queue_frames_))
            return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
        queued_audio_frames_ += frames;
        audio_queue_.push_back(std::move(sample));
        return true;
    }

    [[nodiscard]] bool flush_resampler() {
        for (;;) {
            const auto capacity = swr_get_out_samples(audio_resampler_, 0);
            if (capacity <= 0) return capacity == 0 || fail(map_error(capacity), capacity);
            const auto remaining_frames =
                maximum_audio_queue_frames_ -
                std::min<std::uint64_t>(queued_audio_frames_, maximum_audio_queue_frames_);
            if (static_cast<std::uint64_t>(capacity) > remaining_frames)
                return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
            OutputSample sample;
            sample.kind = NativePortCodecSampleKind::Audio;
            sample.sample_rate = requested_audio_rate_;
            sample.channels = audio_channels_;
            sample.audio.resize(static_cast<std::size_t>(capacity) * audio_channels_);
            auto* output = reinterpret_cast<std::uint8_t*>(sample.audio.data());
            const auto converted = swr_convert(audio_resampler_, &output, capacity, nullptr, 0);
            if (converted < 0) return fail(map_error(converted), converted);
            if (converted == 0) return true;
            sample.audio.resize(static_cast<std::size_t>(converted) * audio_channels_);
            sample.timestamp = next_audio_timestamp_;
            sample.duration = samples_to_ns(converted);
            next_audio_timestamp_ = saturating_add(sample.timestamp, sample.duration);
            const auto frames = sample.audio.size() / audio_channels_;
            queued_audio_frames_ += frames;
            audio_queue_.push_back(std::move(sample));
        }
    }

    [[nodiscard]] bool convert_video_frame() {
        if (frame_->width <= 0 || frame_->height <= 0) return fail_invalid_data();
        const auto stride64 = static_cast<std::uint64_t>(frame_->width) * 4u;
        const auto bytes64 = stride64 * static_cast<std::uint64_t>(frame_->height);
        if (stride64 > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
            bytes64 == 0u ||
            bytes64 > maximum_video_frame_bytes_ ||
            bytes64 > std::numeric_limits<std::size_t>::max())
            return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
        if (video_queue_.size() >= maximum_video_queue_frames_)
            return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
        const auto remaining_bytes =
            maximum_video_queue_bytes_ -
            std::min<std::uint64_t>(queued_video_bytes_, maximum_video_queue_bytes_);
        if (bytes64 > remaining_bytes)
            return fail(NativePortCodecFailure::ResourceExhausted, AVERROR(ENOMEM));
        video_scaler_ = sws_getCachedContext(video_scaler_,
                                             frame_->width,
                                             frame_->height,
                                             static_cast<AVPixelFormat>(frame_->format),
                                             frame_->width,
                                             frame_->height,
                                             AV_PIX_FMT_BGRA,
                                             SWS_BILINEAR,
                                             nullptr,
                                             nullptr,
                                             nullptr);
        if (video_scaler_ == nullptr)
            return fail(NativePortCodecFailure::UnsupportedCodec, AVERROR(EINVAL));
        OutputSample sample;
        sample.kind = NativePortCodecSampleKind::Video;
        sample.width = static_cast<std::uint32_t>(frame_->width);
        sample.height = static_cast<std::uint32_t>(frame_->height);
        sample.stride = static_cast<std::uint32_t>(stride64);
        sample.pixels.resize(static_cast<std::size_t>(bytes64));
        std::uint8_t* planes[] = {
            reinterpret_cast<std::uint8_t*>(sample.pixels.data()), nullptr, nullptr, nullptr};
        int strides[] = {static_cast<int>(sample.stride), 0, 0, 0};
        const auto rows = sws_scale(
            video_scaler_, frame_->data, frame_->linesize, 0, frame_->height, planes, strides);
        if (rows != frame_->height) return fail_invalid_data();
        sample.timestamp = frame_timestamp(*frame_, video_stream_index_, next_video_timestamp_);
        sample.duration = video_duration(*frame_);
        next_video_timestamp_ = saturating_add(sample.timestamp, sample.duration);
        if (!validate_stream_timestamp(sample.timestamp, false)) return false;
        queued_video_bytes_ += bytes64;
        video_queue_.push_back(std::move(sample));
        return true;
    }

    [[nodiscard]] std::uint64_t frame_timestamp(const AVFrame& frame,
                                                const int stream_index,
                                                const std::uint64_t fallback) const noexcept {
        auto timestamp = frame.best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) timestamp = frame.pts;
        if (timestamp == AV_NOPTS_VALUE) return fallback;
        const auto absolute = av_rescale_q(
            timestamp, format_->streams[stream_index]->time_base, nanosecond_time_base);
        if (absolute <= start_time_nanoseconds_) return 0u;
        if (start_time_nanoseconds_ < 0 &&
            absolute > std::numeric_limits<std::int64_t>::max() + start_time_nanoseconds_)
            return std::numeric_limits<std::uint64_t>::max();
        return positive_ns(absolute - start_time_nanoseconds_);
    }

    [[nodiscard]] std::uint64_t video_duration(const AVFrame& frame) const noexcept {
        if (frame.duration > 0)
            return positive_ns(av_rescale_q(frame.duration,
                                            format_->streams[video_stream_index_]->time_base,
                                            nanosecond_time_base));
        const auto rate = av_guess_frame_rate(
            format_, format_->streams[video_stream_index_], const_cast<AVFrame*>(&frame));
        return rate.num > 0 && rate.den > 0
                   ? positive_ns(av_rescale_q(1, av_inv_q(rate), nanosecond_time_base))
                   : 0u;
    }

    [[nodiscard]] std::uint64_t samples_to_ns(const int samples) const noexcept {
        return positive_ns(av_rescale_q(
            samples, AVRational{1, static_cast<int>(requested_audio_rate_)}, nanosecond_time_base));
    }

    [[nodiscard]] bool validate_stream_timestamp(const std::uint64_t timestamp, const bool audio) {
        auto& initialized = audio ? audio_timestamp_initialized_ : video_timestamp_initialized_;
        auto& last = audio ? last_audio_timestamp_ : last_video_timestamp_;
        if (initialized && timestamp < last) return fail_invalid_data();
        initialized = true;
        last = timestamp;
        return true;
    }

    [[nodiscard]] bool fail_invalid_data() {
        return fail(NativePortCodecFailure::InvalidData, AVERROR_INVALIDDATA);
    }

    [[nodiscard]] bool fail(const NativePortCodecFailure failure, const int error) noexcept {
        failure_ = failure;
        error_code_ = static_cast<std::uint32_t>(error == 0 ? AVERROR_UNKNOWN : error);
        return false;
    }

    void write_failure(NativePortCodecReadResult& result) const noexcept {
        result = {};
        result.status = NativePortCodecReadStatus::Failure;
        result.failure =
            failure_ == NativePortCodecFailure::None ? NativePortCodecFailure::Internal : failure_;
        result.provider_error_code = error_code_;
    }

    [[nodiscard]] static NativePortCodecFailure map_error(const int error) noexcept {
        if (error == AVERROR(ENOMEM)) return NativePortCodecFailure::ResourceExhausted;
        if (error == AVERROR(EIO)) return NativePortCodecFailure::ContentRead;
        if (error == AVERROR_DECODER_NOT_FOUND) return NativePortCodecFailure::UnsupportedCodec;
        if (error == AVERROR_DEMUXER_NOT_FOUND) return NativePortCodecFailure::UnsupportedContainer;
        if (error == AVERROR_INVALIDDATA) return NativePortCodecFailure::InvalidData;
        return NativePortCodecFailure::Internal;
    }

    [[nodiscard]] static std::uint64_t positive_ns(const std::int64_t value) noexcept {
        return value > 0 ? static_cast<std::uint64_t>(value) : 0u;
    }

    [[nodiscard]] static std::uint64_t saturating_add(const std::uint64_t left,
                                                      const std::uint64_t right) noexcept {
        return right > std::numeric_limits<std::uint64_t>::max() - left
                   ? std::numeric_limits<std::uint64_t>::max()
                   : left + right;
    }

    NativePortCodecByteSource source_{};
    std::uint64_t position_ = 0u;
    std::uint32_t requested_audio_rate_ = 0u;
    std::uint32_t maximum_audio_queue_frames_ = 0u;
    std::uint32_t maximum_video_queue_frames_ = 0u;
    std::uint64_t maximum_video_frame_bytes_ = 0u;
    std::uint64_t maximum_video_queue_bytes_ = 0u;
    AVIOContext* io_ = nullptr;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* audio_codec_ = nullptr;
    AVCodecContext* video_codec_ = nullptr;
    SwrContext* audio_resampler_ = nullptr;
    SwsContext* video_scaler_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int audio_stream_index_ = -1;
    int video_stream_index_ = -1;
    std::uint32_t audio_channels_ = 0u;
    std::int64_t start_time_nanoseconds_ = 0;
    std::uint64_t duration_nanoseconds_ = 0u;
    std::uint64_t next_audio_timestamp_ = 0u;
    std::uint64_t next_video_timestamp_ = 0u;
    std::uint64_t queued_audio_frames_ = 0u;
    std::uint64_t queued_video_bytes_ = 0u;
    std::deque<OutputSample> audio_queue_;
    std::deque<OutputSample> video_queue_;
    OutputSample current_;
    NativePortCodecFailure failure_ = NativePortCodecFailure::None;
    std::uint32_t error_code_ = 0u;
    std::uint64_t last_audio_timestamp_ = 0u;
    std::uint64_t last_video_timestamp_ = 0u;
    std::uint64_t last_global_timestamp_ = 0u;
    bool audio_timestamp_initialized_ = false;
    bool video_timestamp_initialized_ = false;
    bool global_timestamp_initialized_ = false;
    bool demux_eof_ = false;
    bool audio_flush_sent_ = false;
    bool video_flush_sent_ = false;
    bool audio_drained_ = false;
    bool video_drained_ = false;
    bool inject_sofdec_signature_ = false;
};

void open_ffmpeg(void*,
                 const NativePortCodecOpenRequest* request,
                 NativePortCodecOpenResult* result) noexcept {
    if (result == nullptr) return;
    *result = {};
    if (request == nullptr || request->source.user == nullptr ||
        request->source.read_at == nullptr || request->source.byte_size == 0u ||
        request->source.byte_size > static_cast<std::uint64_t>(INT64_MAX) ||
        request->requested_audio_sample_rate < 8'000u ||
        request->requested_audio_sample_rate > 192'000u ||
        request->maximum_audio_queue_frames == 0u ||
        request->maximum_audio_queue_frames > maximum_provider_audio_queue_frames ||
        request->maximum_video_queue_frames == 0u ||
        request->maximum_video_queue_frames > maximum_provider_video_queue_frames ||
        request->maximum_video_frame_bytes < 4u ||
        request->maximum_video_frame_bytes > maximum_provider_video_frame_bytes ||
        request->maximum_video_queue_bytes < 4u ||
        request->maximum_video_queue_bytes > maximum_provider_video_queue_bytes ||
        request->maximum_video_frame_bytes > request->maximum_video_queue_bytes ||
        request->require_audio > 1u || request->require_video > 1u ||
        (request->require_audio == 0u && request->require_video == 0u)) {
        result->failure = NativePortCodecFailure::InvalidContract;
        return;
    }
    try {
        auto decoder = std::make_unique<FfmpegDecoder>(*request);
        if (!decoder->initialize()) {
            result->failure = decoder->failure();
            result->provider_error_code = decoder->error_code();
            return;
        }
        decoder->describe(*result);
        static_cast<void>(decoder.release());
    } catch (const std::bad_alloc&) {
        result->failure = NativePortCodecFailure::ResourceExhausted;
        result->provider_error_code = static_cast<std::uint32_t>(AVERROR(ENOMEM));
    } catch (...) {
        result->failure = NativePortCodecFailure::Internal;
        result->provider_error_code = static_cast<std::uint32_t>(AVERROR_BUG);
    }
}

void read_ffmpeg(void* decoder, NativePortCodecReadResult* result) noexcept {
    if (result == nullptr) return;
    if (decoder == nullptr) {
        *result = {};
        result->failure = NativePortCodecFailure::InvalidContract;
        return;
    }
    try {
        static_cast<FfmpegDecoder*>(decoder)->read(*result);
    } catch (const std::bad_alloc&) {
        *result = {};
        result->failure = NativePortCodecFailure::ResourceExhausted;
        result->provider_error_code = static_cast<std::uint32_t>(AVERROR(ENOMEM));
    } catch (...) {
        *result = {};
        result->failure = NativePortCodecFailure::Internal;
        result->provider_error_code = static_cast<std::uint32_t>(AVERROR_BUG);
    }
}

void close_ffmpeg(void* decoder) noexcept {
    delete static_cast<FfmpegDecoder*>(decoder);
}

} // namespace

const NativePortCodecProvider& native_port_ffmpeg_codec_provider() noexcept {
    static const NativePortCodecProvider provider = [] {
        NativePortCodecProvider value;
        value.structure_size = native_port_codec_provider_structure_size;
        constexpr char name[] = "ffmpeg-lgpl";
        std::memcpy(value.provider_name, name, sizeof(name));
        value.open = &open_ffmpeg;
        value.read_next = &read_ffmpeg;
        value.close = &close_ffmpeg;
        return value;
    }();
    return provider;
}

} // namespace katana::runtime
