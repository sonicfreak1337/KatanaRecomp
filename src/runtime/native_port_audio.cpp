#include "katana/runtime/native_port_audio.hpp"

#include <algorithm>
#include <limits>
#include <list>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(disable : 6553)
#endif
#define NOMINMAX
#include <windows.h>

#include <mmeapi.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::uint32_t maximum_audio_queue_budget_frames = 192'000u * 60u;

void validate_config(const NativePortAudioConfig& config) {
    if (config.format.sample_rate < 8'000u || config.format.sample_rate > 192'000u ||
        (config.format.channels != 1u && config.format.channels != 2u) ||
        config.maximum_queued_frames == 0u ||
        config.maximum_queued_frames > maximum_audio_queue_budget_frames)
        throw std::invalid_argument("native-port-audio-config");
}

void saturating_add(std::uint64_t& destination, const std::uint64_t value) noexcept {
    destination = value > std::numeric_limits<std::uint64_t>::max() - destination
                      ? std::numeric_limits<std::uint64_t>::max()
                      : destination + value;
}

} // namespace

class NativePortAudioStream::Impl final {
  public:
    explicit Impl(const NativePortAudioConfig& config)
        : config_(config), owner_thread_(std::this_thread::get_id()) {
        validate_config(config_);
#ifdef _WIN32
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = config_.format.channels;
        format.nSamplesPerSec = config_.format.sample_rate;
        format.wBitsPerSample = 16u;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8u);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const auto result = waveOutOpen(&device_, WAVE_MAPPER, &format, 0u, 0u, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) fail(result, "open");
#else
        fail(1u, "unsupported-host");
#endif
    }

    ~Impl() {
        static_cast<void>(stop_unchecked());
    }

    [[nodiscard]] const NativePortAudioFormat& format() const noexcept {
        return config_.format;
    }

    [[nodiscard]] bool submit(const std::span<const std::int16_t> samples) {
        require_owner_thread();
        if (state_ == NativePortAudioState::Stopped || state_ == NativePortAudioState::Failed)
            throw std::logic_error("native-port-audio-not-running");
        if (state_ == NativePortAudioState::Paused)
            throw std::logic_error("native-port-audio-paused");
        if (samples.empty() || samples.size() % config_.format.channels != 0u)
            throw std::invalid_argument("native-port-audio-frame-layout");
        const auto frames = samples.size() / config_.format.channels;
        if (frames > std::numeric_limits<std::uint32_t>::max() ||
            frames > config_.maximum_queued_frames)
            throw std::out_of_range("native-port-audio-buffer-size");
        poll();
        if (queued_frames_ > config_.maximum_queued_frames - frames) return false;
#ifdef _WIN32
        if (samples.size_bytes() > std::numeric_limits<DWORD>::max())
            throw std::out_of_range("native-port-audio-buffer-bytes");
        auto block = std::make_unique<Block>();
        block->frames = static_cast<std::uint32_t>(frames);
        block->samples.assign(samples.begin(), samples.end());
        block->header.lpData = reinterpret_cast<LPSTR>(block->samples.data());
        block->header.dwBufferLength = static_cast<DWORD>(samples.size_bytes());
        // Allocate the list node before the driver can retain WAVEHDR or PCM
        // storage. No throwing allocation may follow a successful write.
        blocks_.push_back(std::move(block));
        auto& submitted = *blocks_.back();
        auto result = waveOutPrepareHeader(device_, &submitted.header, sizeof(submitted.header));
        if (result != MMSYSERR_NOERROR) {
            blocks_.pop_back();
            fail(result, "prepare");
        }
        submitted.prepared = true;
        result = waveOutWrite(device_, &submitted.header, sizeof(submitted.header));
        if (result != MMSYSERR_NOERROR) {
            const auto unprepare =
                waveOutUnprepareHeader(device_, &submitted.header, sizeof(submitted.header));
            if (unprepare == MMSYSERR_NOERROR) {
                submitted.prepared = false;
                blocks_.pop_back();
            }
            // If the driver still owns the header, retain it in blocks_ so
            // stop_unchecked() can reset and release it deterministically.
            fail(result, "submit");
        }
#endif
        saturating_add(submitted_buffers_, 1u);
        saturating_add(submitted_frames_, frames);
        saturating_add(queued_frames_, frames);
        state_ = NativePortAudioState::Running;
        return true;
    }

    void poll() {
        require_owner_thread();
#ifdef _WIN32
        while (!blocks_.empty()) {
            auto& block = *blocks_.front();
            if (block.prepared) {
                const auto result =
                    waveOutUnprepareHeader(device_, &block.header, sizeof(block.header));
                if (result == WAVERR_STILLPLAYING) break;
                if (result != MMSYSERR_NOERROR) fail(result, "unprepare");
                block.prepared = false;
            }
            saturating_add(completed_frames_, block.frames);
            queued_frames_ = queued_frames_ >= block.frames ? queued_frames_ - block.frames : 0u;
            blocks_.pop_front();
        }
#endif
    }

    void refresh_playback_position() {
        require_owner_thread();
#ifdef _WIN32
        update_playback_position();
#endif
    }

    void pause() {
        require_owner_thread();
        if (state_ == NativePortAudioState::Stopped || state_ == NativePortAudioState::Failed ||
            state_ == NativePortAudioState::Paused)
            return;
#ifdef _WIN32
        const auto result = waveOutPause(device_);
        if (result != MMSYSERR_NOERROR) fail(result, "pause");
#endif
        state_ = NativePortAudioState::Paused;
    }

    void resume() {
        require_owner_thread();
        if (state_ == NativePortAudioState::Stopped || state_ == NativePortAudioState::Failed)
            throw std::logic_error("native-port-audio-stopped");
        if (state_ != NativePortAudioState::Paused) return;
#ifdef _WIN32
        const auto result = waveOutRestart(device_);
        if (result != MMSYSERR_NOERROR) fail(result, "resume");
#endif
        state_ = NativePortAudioState::Running;
    }

    void stop() {
        require_owner_thread();
        const auto error = stop_unchecked();
        if (error != 0u)
            throw std::runtime_error("native-port-audio-stop:" + std::to_string(error));
    }

    [[nodiscard]] std::uint32_t stop_unchecked() noexcept {
        if (state_ == NativePortAudioState::Stopped) return 0u;
        std::uint32_t first_error = 0u;
        const auto observe_error = [&](const std::uint32_t error) {
            if (error != 0u && first_error == 0u) first_error = error;
        };
#ifdef _WIN32
        if (device_ != nullptr) {
            observe_error(waveOutReset(device_));
            for (auto& block : blocks_) {
                if (!block->prepared) continue;
                const auto result =
                    waveOutUnprepareHeader(device_, &block->header, sizeof(block->header));
                observe_error(result);
                if (result == MMSYSERR_NOERROR) block->prepared = false;
            }
            const auto close_result = waveOutClose(device_);
            observe_error(close_result);
            if (close_result == MMSYSERR_NOERROR) {
                // A successful close is the final ownership boundary: the
                // driver no longer retains any submitted WAVEHDR storage.
                blocks_.clear();
            } else {
                // The driver may still reference submitted storage after a
                // failed close. Leak it rather than freeing live DMA data.
                for (auto& block : blocks_)
                    static_cast<void>(block.release());
                blocks_.clear();
            }
            device_ = nullptr;
        }
#endif
        queued_frames_ = 0u;
        if (first_error != 0u) {
            error_code_ = first_error;
            state_ = NativePortAudioState::Failed;
        } else {
            state_ = NativePortAudioState::Stopped;
        }
        return first_error;
    }

    [[nodiscard]] NativePortAudioSnapshot snapshot() const noexcept {
        return {state_,
                submitted_buffers_,
                submitted_frames_,
                completed_frames_,
                queued_frames_,
                playback_position_queries_,
                error_code_};
    }

    [[nodiscard]] std::uint64_t playback_position_frames() const noexcept {
        return played_frames_;
    }

  private:
#ifdef _WIN32
    void update_playback_position() {
        if (device_ == nullptr) return;
        saturating_add(playback_position_queries_, 1u);
        MMTIME position{};
        position.wType = TIME_SAMPLES;
        const auto result = waveOutGetPosition(device_, &position, sizeof(position));
        if (result != MMSYSERR_NOERROR) fail(result, "position");
        if (position.wType != TIME_SAMPLES) {
            // WAVEHDR completion is a conservative playback lower bound when
            // a legacy driver cannot expose its sample clock.
            played_frames_ = std::max(played_frames_, completed_frames_);
            return;
        }
        auto raw = static_cast<std::uint32_t>(position.u.sample);
        if (device_position_initialized_) {
            if (raw < last_device_position_raw_) {
                if (last_device_position_raw_ - raw > 0x8000'0000u)
                    device_position_epoch_ += std::uint64_t{1u} << 32u;
                else
                    raw = last_device_position_raw_;
            }
        } else {
            device_position_initialized_ = true;
        }
        last_device_position_raw_ = raw;
        const auto device_frames = device_position_epoch_ + raw;
        played_frames_ = std::min(submitted_frames_,
                                  std::max(played_frames_, device_frames));
    }
#endif

    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_)
            throw std::logic_error("native-port-audio-thread-violation");
    }

    [[noreturn]] void fail(const std::uint32_t code, const char* operation) {
        error_code_ = code == 0u ? 1u : code;
        state_ = NativePortAudioState::Failed;
        throw std::runtime_error(std::string("native-port-audio-") + operation + ":" +
                                 std::to_string(error_code_));
    }

#ifdef _WIN32
    struct Block final {
        WAVEHDR header{};
        std::vector<std::int16_t> samples;
        std::uint32_t frames = 0u;
        bool prepared = false;
    };
    HWAVEOUT device_ = nullptr;
    std::list<std::unique_ptr<Block>> blocks_;
#endif
    NativePortAudioConfig config_;
    std::thread::id owner_thread_;
    NativePortAudioState state_ = NativePortAudioState::Ready;
    std::uint64_t submitted_buffers_ = 0u;
    std::uint64_t submitted_frames_ = 0u;
    std::uint64_t completed_frames_ = 0u;
    std::uint64_t queued_frames_ = 0u;
    std::uint64_t played_frames_ = 0u;
    std::uint64_t playback_position_queries_ = 0u;
#ifdef _WIN32
    std::uint64_t device_position_epoch_ = 0u;
    std::uint32_t last_device_position_raw_ = 0u;
    bool device_position_initialized_ = false;
#endif
    std::uint32_t error_code_ = 0u;
};

NativePortAudioStream::NativePortAudioStream(const NativePortAudioConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

NativePortAudioStream::~NativePortAudioStream() = default;

const NativePortAudioFormat& NativePortAudioStream::format() const noexcept {
    return impl_->format();
}

bool NativePortAudioStream::submit_pcm_s16(
    const std::span<const std::int16_t> interleaved_samples) {
    return impl_->submit(interleaved_samples);
}

void NativePortAudioStream::poll() {
    impl_->poll();
}
void NativePortAudioStream::refresh_playback_position() {
    impl_->refresh_playback_position();
}
void NativePortAudioStream::pause() {
    impl_->pause();
}
void NativePortAudioStream::resume() {
    impl_->resume();
}
void NativePortAudioStream::stop() {
    impl_->stop();
}
NativePortAudioSnapshot NativePortAudioStream::snapshot() const noexcept {
    return impl_->snapshot();
}
std::uint64_t NativePortAudioStream::playback_position_frames() const noexcept {
    return impl_->playback_position_frames();
}

} // namespace katana::runtime
