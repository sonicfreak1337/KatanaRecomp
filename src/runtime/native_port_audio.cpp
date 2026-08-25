#include "katana/runtime/native_port_audio.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
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
constexpr std::size_t native_audio_block_pool_size = 16u;
constexpr std::uint32_t native_audio_pooled_block_frames = 16'384u;

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
#ifdef _WIN32
    struct Block;
    struct PendingBlock;
#endif

  public:
    explicit Impl(const NativePortAudioConfig& config)
        : config_(config), owner_thread_(std::this_thread::get_id()) {
        validate_config(config_);
#ifdef _WIN32
        pool_.reserve(native_audio_block_pool_size);
        free_pool_indices_.reserve(native_audio_block_pool_size);
        for (std::size_t index = 0u; index < native_audio_block_pool_size;
             ++index) {
            auto block = std::make_unique<Block>();
            block->samples.resize(
                static_cast<std::size_t>(native_audio_pooled_block_frames) *
                config_.format.channels);
            block->header.lpData =
                reinterpret_cast<LPSTR>(block->samples.data());
            pool_.push_back(std::move(block));
            free_pool_indices_.push_back(native_audio_block_pool_size - 1u -
                                         index);
        }
        const auto maximum_oversized_blocks =
            static_cast<std::size_t>(config_.maximum_queued_frames /
                                     (native_audio_pooled_block_frames + 1u)) +
            1u;
        pending_.reserve(native_audio_block_pool_size +
                         maximum_oversized_blocks);
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
        PendingBlock pending;
        if (frames <= native_audio_pooled_block_frames &&
            !free_pool_indices_.empty()) {
            pending.pool_index = free_pool_indices_.back();
            free_pool_indices_.pop_back();
            pending.block = pool_[*pending.pool_index].get();
            std::copy(samples.begin(), samples.end(), pending.block->samples.begin());
        } else {
            pending.owned = std::make_unique<Block>();
            pending.owned->samples.assign(samples.begin(), samples.end());
            pending.block = pending.owned.get();
            pending.block->header.lpData =
                reinterpret_cast<LPSTR>(pending.block->samples.data());
        }
        pending.block->frames = static_cast<std::uint32_t>(frames);
        const auto buffer_bytes = static_cast<DWORD>(samples.size_bytes());
        if (pending.block->prepared &&
            pending.block->header.dwBufferLength != buffer_bytes) {
            const auto unprepare = waveOutUnprepareHeader(
                device_, &pending.block->header, sizeof(pending.block->header));
            if (unprepare != MMSYSERR_NOERROR) {
                if (pending.pool_index.has_value())
                    free_pool_indices_.push_back(*pending.pool_index);
                fail(unprepare, "resize-unprepare");
            }
            pending.block->prepared = false;
        }
        pending.block->header.dwBufferLength = buffer_bytes;
        pending_.push_back(std::move(pending));
        auto& submitted = *pending_.back().block;
        auto result = submitted.prepared
                          ? MMSYSERR_NOERROR
                          : waveOutPrepareHeader(
                                device_, &submitted.header, sizeof(submitted.header));
        if (result != MMSYSERR_NOERROR) {
            release_failed_submission(pending_.back());
            pending_.pop_back();
            fail(result, "prepare");
        }
        submitted.prepared = true;
        submitted.header.dwFlags &= ~WHDR_DONE;
        result = waveOutWrite(device_, &submitted.header, sizeof(submitted.header));
        if (result != MMSYSERR_NOERROR) {
            const auto unprepare =
                waveOutUnprepareHeader(device_, &submitted.header, sizeof(submitted.header));
            if (unprepare == MMSYSERR_NOERROR) {
                submitted.prepared = false;
                release_failed_submission(pending_.back());
                pending_.pop_back();
            }
            // If the driver still owns the header, retain it in pending_ so
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
        for (auto& pending : pending_) {
            auto& block = *pending.block;
            if ((block.header.dwFlags & WHDR_DONE) == 0u) break;
            if (pending.owned && block.prepared) {
                const auto result = waveOutUnprepareHeader(
                    device_, &block.header, sizeof(block.header));
                if (result == WAVERR_STILLPLAYING) break;
                if (result != MMSYSERR_NOERROR) fail(result, "unprepare");
                block.prepared = false;
            }
            saturating_add(completed_frames_, block.frames);
            queued_frames_ = queued_frames_ >= block.frames ? queued_frames_ - block.frames : 0u;
            if (pending.pool_index.has_value())
                free_pool_indices_.push_back(*pending.pool_index);
            pending.block = nullptr;
        }
        if (const auto first_live = std::ranges::find_if(
                pending_, [](const auto& value) { return value.block != nullptr; });
            first_live != pending_.begin()) {
            pending_.erase(pending_.begin(), first_live);
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
            for (auto& block : pool_) {
                if (!block->prepared) continue;
                const auto result =
                    waveOutUnprepareHeader(device_, &block->header, sizeof(block->header));
                observe_error(result);
                if (result == MMSYSERR_NOERROR) block->prepared = false;
            }
            for (auto& pending : pending_) {
                if (!pending.owned || !pending.block->prepared) continue;
                const auto result = waveOutUnprepareHeader(
                    device_, &pending.block->header, sizeof(pending.block->header));
                observe_error(result);
                if (result == MMSYSERR_NOERROR) pending.block->prepared = false;
            }
            const auto close_result = waveOutClose(device_);
            observe_error(close_result);
            if (close_result == MMSYSERR_NOERROR) {
                // A successful close is the final ownership boundary: the
                // driver no longer retains any submitted WAVEHDR storage.
                pending_.clear();
                pool_.clear();
            } else {
                // The driver may still reference submitted storage after a
                // failed close. Leak it rather than freeing live DMA data.
                for (auto& block : pool_)
                    static_cast<void>(block.release());
                for (auto& pending : pending_)
                    if (pending.owned)
                        static_cast<void>(pending.owned.release());
                pending_.clear();
                pool_.clear();
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
                played_frames_,
                playback_position_queries_,
                error_code_};
    }

    [[nodiscard]] std::uint64_t playback_position_frames() const noexcept {
        return played_frames_;
    }

  private:
#ifdef _WIN32
    void release_failed_submission(PendingBlock& pending) noexcept {
        if (pending.pool_index.has_value())
            free_pool_indices_.push_back(*pending.pool_index);
        pending.block = nullptr;
    }
#endif

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
    struct PendingBlock final {
        Block* block = nullptr;
        std::optional<std::size_t> pool_index;
        std::unique_ptr<Block> owned;
    };
    HWAVEOUT device_ = nullptr;
    std::vector<std::unique_ptr<Block>> pool_;
    std::vector<std::size_t> free_pool_indices_;
    std::vector<PendingBlock> pending_;
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
