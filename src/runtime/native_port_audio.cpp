#include "katana/runtime/native_port_audio.hpp"

#include "native_port_audio_execution_domain.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
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

[[nodiscard]] bool background_test_mode_requested() noexcept {
    static const bool requested = [] {
        const auto* const value = std::getenv("KATANA_PORT_BACKGROUND_TEST");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return requested;
}

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

// This is the old host endpoint implementation.  It deliberately remains a
// single-thread owner, but its owner is now the process-wide audio domain
// worker rather than the public NativePortAudioStream facade/caller.
class NativePortAudioEndpointCore final {
#ifdef _WIN32
    struct Block;
    struct PendingBlock;
    struct CompletionWakeState final {
        static constexpr std::uint32_t closed_bit = 0x8000'0000u;
        static constexpr std::uint32_t in_flight_mask = ~closed_bit;

        [[nodiscard]] bool try_enter() noexcept {
            auto observed = lifecycle.load(std::memory_order_acquire);
            for (;;) {
                if ((observed & closed_bit) != 0u ||
                    (observed & in_flight_mask) == in_flight_mask)
                    return false;
                if (lifecycle.compare_exchange_weak(
                        observed, observed + 1u, std::memory_order_acq_rel,
                        std::memory_order_acquire))
                    return true;
            }
        }

        void leave() noexcept {
            const auto previous =
                lifecycle.fetch_sub(1u, std::memory_order_acq_rel);
            if ((previous & closed_bit) != 0u &&
                (previous & in_flight_mask) == 1u)
                lifecycle.notify_all();
        }

        void close_and_wait() noexcept {
            auto observed =
                lifecycle.fetch_or(closed_bit, std::memory_order_acq_rel) |
                closed_bit;
            while ((observed & in_flight_mask) != 0u) {
                lifecycle.wait(observed, std::memory_order_acquire);
                observed = lifecycle.load(std::memory_order_acquire);
            }
        }

        std::atomic<std::uint32_t> lifecycle{0u};
        std::atomic<NativePortAudioExecutionDomain*> execution_domain{nullptr};
    };
#endif

  public:
    NativePortAudioEndpointCore(
        const NativePortAudioConfig& config,
        NativePortAudioExecutionDomain& execution_domain)
        : config_(config), owner_thread_(std::this_thread::get_id()) {
        validate_config(config_);
#ifdef _WIN32
        completion_wake_ = std::make_unique<CompletionWakeState>();
        completion_wake_->execution_domain.store(
            &execution_domain, std::memory_order_release);
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
        const auto result = waveOutOpen(
            &device_, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(
                &NativePortAudioEndpointCore::wave_out_callback),
            reinterpret_cast<DWORD_PTR>(completion_wake_.get()),
            CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) fail(result, "open");
#else
        fail(1u, "unsupported-host");
#endif
    }

    ~NativePortAudioEndpointCore() {
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
        if (background_test_mode_requested())
            std::fill_n(pending.block->samples.begin(), samples.size(), 0);
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
            if (completion_wake_ != nullptr) {
                // Close admission first, then wait outside the callback
                // hotpath for every callback which entered before the close.
                // Only after this fence may the execution-domain lifetime be
                // severed or the host device reset.
                completion_wake_->close_and_wait();
                completion_wake_->execution_domain.store(
                    nullptr, std::memory_order_release);
            }
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
                // driver retains neither submitted WAVEHDR storage nor the
                // callback instance, so the closed wake state may be freed.
                pending_.clear();
                pool_.clear();
                completion_wake_.reset();
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
                // The driver may still invoke its registered callback. Keep
                // an inert, null-domain instance alive alongside leaked DMA
                // storage rather than exposing Core/Domain lifetime.
                static_cast<void>(completion_wake_.release());
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
    static void CALLBACK wave_out_callback(
        HWAVEOUT,
        const UINT message,
        const DWORD_PTR instance,
        DWORD_PTR,
        DWORD_PTR) noexcept {
        if (message != WOM_DONE || instance == 0u) return;
        // The OS callback is never an audio executor. It only advances the
        // domain's atomic wake epoch; poll/retry remains on the sole consumer.
        auto* const wake = reinterpret_cast<CompletionWakeState*>(instance);
        if (!wake->try_enter()) return;
        auto* const domain =
            wake->execution_domain.load(std::memory_order_acquire);
        if (domain != nullptr) domain->request_consumer_service();
        wake->leave();
    }

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
    std::unique_ptr<CompletionWakeState> completion_wake_;
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

namespace {

enum class NativePortAudioStreamOpcode : std::uint16_t {
    Construct = 1u,
    SubmitPcmS16 = 2u,
    Poll = 3u,
    RefreshPlaybackPosition = 4u,
    Pause = 5u,
    Resume = 6u,
    Stop = 7u,
    Destroy = 8u,
};

std::atomic<std::uint64_t> next_legacy_audio_frame_index{0u};

[[nodiscard]] std::uint64_t next_legacy_frame_index() noexcept {
    return next_legacy_audio_frame_index.fetch_add(1u,
                                                   std::memory_order_relaxed);
}

} // namespace

class NativePortAudioStream::Impl final {
  public:
    Impl(const NativePortAudioConfig& config,
         const NativePortAudioExecutionDomainTarget target)
        : config_(config), target_(target),
          domain_(NativePortAudioExecutionDomain::acquire(
              NativePortAudioExecutionDomainConfig{
                  normalized_queue_config(config.command_queue)})) {
        validate_config(config_);
        const auto registered = domain_->register_target(
            target_, this, &NativePortAudioStream::Impl::execute,
            &NativePortAudioStream::Impl::cleanup_worker_state,
            &NativePortAudioStream::Impl::service_worker_completion);
        if (!registered.has_value())
            throw std::runtime_error("native-port-audio-target-register");
        handle_ = *registered;
        const auto result = dispatch(NativePortAudioStreamOpcode::Construct,
                                     {}, compatible_frame_index(
                                             next_legacy_frame_index()));
        if (!result.completed()) {
            // Construct may already have installed a worker-owned endpoint.
            // A failed ACK therefore takes the same consumer-side cleanup
            // path as every other terminal failure; unregistering here would
            // disarm that cleanup while leaving Core owned by the producer.
            domain_->shutdown();
            handle_ = {};
            throw NativePortAudioExecutionDomainError(
                result.failure ==
                        NativePortAudioExecutionDomainFailure::None
                    ? NativePortAudioExecutionDomainFailure::TargetExecutionFailed
                    : result.failure,
                result.queue_failure, result.command_sequence);
        }
    }

    ~Impl() {
        if (!domain_ || !handle_.valid()) return;
        const auto result = dispatch(NativePortAudioStreamOpcode::Destroy,
                                     {}, compatible_frame_index(
                                             next_legacy_frame_index()));
        if (result.completed()) {
            if (!domain_->unregister_target(handle_, this)) domain_->shutdown();
        } else {
            // The shared consumer drains/cancels, invokes the registered
            // noexcept cleanup exactly once, and joins before this facade's
            // storage can disappear.  Never leak or producer-destroy Core.
            domain_->shutdown();
        }
        handle_ = {};
    }

    [[nodiscard]] const NativePortAudioFormat& format() const noexcept {
        return config_.format;
    }

    [[nodiscard]] bool submit(
        const std::span<const std::int16_t> samples,
        const std::uint64_t frame_index) {
        if (samples.empty() ||
            samples.size() % config_.format.channels != 0u)
            throw std::invalid_argument("native-port-audio-frame-layout");
        if (samples.size_bytes() > std::numeric_limits<std::uint32_t>::max())
            throw std::out_of_range("native-port-audio-buffer-size");
        const auto result = dispatch(
            NativePortAudioStreamOpcode::SubmitPcmS16,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(samples.data()),
                samples.size_bytes()),
            frame_index);
        require_completed(result, "submit");
        if (!result.has_ack || result.ack.result_size != 1u)
            throw std::runtime_error("native-port-audio-submit-ack");
        return std::to_integer<std::uint8_t>(result.ack.bytes[0]) != 0u;
    }

    void poll() { require_completed(dispatch(NativePortAudioStreamOpcode::Poll,
                                             {}, next_legacy_frame_index()),
                                     "poll"); }

    void refresh_playback_position() {
        require_completed(
            dispatch(NativePortAudioStreamOpcode::RefreshPlaybackPosition, {},
                     next_legacy_frame_index()),
            "refresh-playback-position");
    }

    void pause() {
        require_completed(dispatch(NativePortAudioStreamOpcode::Pause, {},
                                   next_legacy_frame_index()),
                          "pause");
    }

    void resume() {
        require_completed(dispatch(NativePortAudioStreamOpcode::Resume, {},
                                   next_legacy_frame_index()),
                          "resume");
    }

    void stop() {
        require_completed(dispatch(NativePortAudioStreamOpcode::Stop, {},
                                   next_legacy_frame_index()),
                          "stop");
    }

    [[nodiscard]] NativePortAudioSnapshot snapshot() const noexcept {
        NativePortAudioSnapshot result;
        result.state = state_.load(std::memory_order_acquire);
        result.submitted_buffers = submitted_buffers_.load(std::memory_order_acquire);
        result.submitted_frames = submitted_frames_.load(std::memory_order_acquire);
        result.completed_frames = completed_frames_.load(std::memory_order_acquire);
        result.queued_frames = queued_frames_.load(std::memory_order_acquire);
        result.playback_position_frames =
            playback_position_frames_.load(std::memory_order_acquire);
        result.playback_position_queries =
            playback_position_queries_.load(std::memory_order_acquire);
        result.error_code = error_code_.load(std::memory_order_acquire);
        return result;
    }

    [[nodiscard]] std::uint64_t playback_position_frames() const noexcept {
        return playback_position_frames_.load(std::memory_order_acquire);
    }

  private:
    [[nodiscard]] static NativePortAudioCommandQueueConfig
    normalized_queue_config(NativePortAudioCommandQueueConfig config) {
        if (native_port_audio_serial_reference_requested())
            config.mode = NativePortAudioCommandQueueMode::SerialReference;
        return config;
    }

    [[nodiscard]] std::uint64_t compatible_frame_index(
        const std::uint64_t requested) const noexcept {
        const auto last = domain_->last_frame_index_nonblocking();
        return last.has_value() ? std::max(requested, *last) : requested;
    }
    using AckResult = NativePortAudioCommandAckResult;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch(
        const NativePortAudioStreamOpcode opcode,
        const std::span<const std::byte> payload,
        const std::uint64_t frame_index) const noexcept {
        return domain_->dispatch_sync(
            handle_, static_cast<std::uint16_t>(opcode), payload,
            compatible_frame_index(frame_index));
    }

    static void execute(void* const object,
                        const std::uint16_t raw_opcode,
                        const std::span<const std::byte> payload,
                        AckResult& result) noexcept {
        auto& self = *static_cast<NativePortAudioStream::Impl*>(object);
        try {
            const auto opcode = static_cast<NativePortAudioStreamOpcode>(raw_opcode);
            switch (opcode) {
            case NativePortAudioStreamOpcode::Construct:
                if (!payload.empty() || self.core_ != nullptr)
                    throw std::logic_error("native-port-audio-construct");
                self.core_ = std::make_unique<NativePortAudioEndpointCore>(
                    self.config_, *self.domain_);
                self.publish_snapshot();
                break;
            case NativePortAudioStreamOpcode::SubmitPcmS16: {
                if (self.core_ == nullptr || payload.empty() ||
                    payload.size() % sizeof(std::int16_t) != 0u)
                    throw std::invalid_argument("native-port-audio-submit-payload");
                const auto sample_count = payload.size() / sizeof(std::int16_t);
                if (reinterpret_cast<std::uintptr_t>(payload.data()) %
                        alignof(std::int16_t) !=
                    0u)
                    throw std::invalid_argument("native-port-audio-payload-alignment");
                const auto* const samples =
                    reinterpret_cast<const std::int16_t*>(payload.data());
                const auto accepted = self.core_->submit(
                    std::span<const std::int16_t>(samples, sample_count));
                result.bytes[0] = accepted ? std::byte{1u} : std::byte{0u};
                result.result_size = 1u;
                self.publish_snapshot();
                break;
            }
            case NativePortAudioStreamOpcode::Poll:
                if (self.core_ == nullptr || !payload.empty())
                    throw std::logic_error("native-port-audio-poll");
                self.core_->poll();
                self.publish_snapshot();
                break;
            case NativePortAudioStreamOpcode::RefreshPlaybackPosition:
                if (self.core_ == nullptr || !payload.empty())
                    throw std::logic_error("native-port-audio-refresh");
                self.core_->refresh_playback_position();
                self.publish_snapshot();
                break;
            case NativePortAudioStreamOpcode::Pause:
                if (self.core_ == nullptr || !payload.empty())
                    throw std::logic_error("native-port-audio-pause");
                self.core_->pause();
                self.publish_snapshot();
                break;
            case NativePortAudioStreamOpcode::Resume:
                if (self.core_ == nullptr || !payload.empty())
                    throw std::logic_error("native-port-audio-resume");
                self.core_->resume();
                self.publish_snapshot();
                break;
            case NativePortAudioStreamOpcode::Stop:
                if (self.core_ == nullptr || !payload.empty())
                    throw std::logic_error("native-port-audio-stop");
                self.core_->stop();
                self.publish_snapshot();
                break;
            case NativePortAudioStreamOpcode::Destroy:
                if (!payload.empty())
                    throw std::logic_error("native-port-audio-destroy");
                self.core_.reset();
                self.state_.store(NativePortAudioState::Stopped,
                                  std::memory_order_release);
                self.queued_frames_.store(0u, std::memory_order_release);
                break;
            default:
                throw std::invalid_argument("native-port-audio-opcode");
            }
            result.status = NativePortAudioCommandAckStatus::Completed;
        } catch (...) {
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 1u;
            self.state_.store(NativePortAudioState::Failed,
                              std::memory_order_release);
            self.error_code_.store(result.error_code, std::memory_order_release);
        }
    }

    static void cleanup_worker_state(void* const object) noexcept {
        auto& self = *static_cast<NativePortAudioStream::Impl*>(object);
        self.core_.reset();
        if (self.state_.load(std::memory_order_acquire) !=
            NativePortAudioState::Stopped)
            self.state_.store(NativePortAudioState::Failed,
                              std::memory_order_release);
        self.queued_frames_.store(0u, std::memory_order_release);
    }

    static std::uint32_t
    service_worker_completion(void* const object) noexcept {
        auto& self = *static_cast<NativePortAudioStream::Impl*>(object);
        if (self.core_ == nullptr) return 0u;
        try {
            self.core_->poll();
            self.publish_snapshot();
            return 0u;
        } catch (...) {
            self.state_.store(NativePortAudioState::Failed,
                              std::memory_order_release);
            self.error_code_.store(1u, std::memory_order_release);
            return 1u;
        }
    }

    void publish_snapshot() noexcept {
        if (core_ == nullptr) return;
        const auto value = core_->snapshot();
        state_.store(value.state, std::memory_order_release);
        submitted_buffers_.store(value.submitted_buffers, std::memory_order_release);
        submitted_frames_.store(value.submitted_frames, std::memory_order_release);
        completed_frames_.store(value.completed_frames, std::memory_order_release);
        queued_frames_.store(value.queued_frames, std::memory_order_release);
        playback_position_frames_.store(value.playback_position_frames,
                                        std::memory_order_release);
        playback_position_queries_.store(value.playback_position_queries,
                                         std::memory_order_release);
        error_code_.store(value.error_code, std::memory_order_release);
    }

    static void require_completed(
        const NativePortAudioExecutionDomainDispatchResult& result,
        const char* operation) {
        static_cast<void>(operation);
        if (result.completed()) return;
        throw NativePortAudioExecutionDomainError(
            result.failure == NativePortAudioExecutionDomainFailure::None
                ? NativePortAudioExecutionDomainFailure::TargetExecutionFailed
                : result.failure,
            result.queue_failure, result.command_sequence);
    }

    NativePortAudioConfig config_;
    NativePortAudioExecutionDomainTarget target_;
    std::shared_ptr<NativePortAudioExecutionDomain> domain_;
    NativePortAudioExecutionDomainTargetHandle handle_{};
    std::unique_ptr<NativePortAudioEndpointCore> core_;
    std::atomic<NativePortAudioState> state_{NativePortAudioState::Ready};
    std::atomic<std::uint64_t> submitted_buffers_{0u};
    std::atomic<std::uint64_t> submitted_frames_{0u};
    std::atomic<std::uint64_t> completed_frames_{0u};
    std::atomic<std::uint64_t> queued_frames_{0u};
    std::atomic<std::uint64_t> playback_position_frames_{0u};
    std::atomic<std::uint64_t> playback_position_queries_{0u};
    std::atomic<std::uint32_t> error_code_{0u};
};

NativePortAudioStream::NativePortAudioStream(
    const NativePortAudioConfig& config)
    : impl_(std::make_unique<Impl>(
          config, NativePortAudioExecutionDomainTarget::HostOutput)) {}

NativePortAudioStream::NativePortAudioStream(
    const NativePortAudioConfig& config,
    MovieTargetTag)
    : impl_(std::make_unique<Impl>(
          config, NativePortAudioExecutionDomainTarget::Movie)) {}

NativePortAudioStream::~NativePortAudioStream() = default;

const NativePortAudioFormat& NativePortAudioStream::format() const noexcept {
    return impl_->format();
}

bool NativePortAudioStream::submit_pcm_s16(
    const std::span<const std::int16_t> interleaved_samples) {
    return impl_->submit(interleaved_samples, next_legacy_frame_index());
}

bool NativePortAudioStream::submit_pcm_s16(
    const std::span<const std::int16_t> interleaved_samples,
    const std::uint64_t frame_index) {
    return impl_->submit(interleaved_samples, frame_index);
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

std::unique_ptr<NativePortAudioStream>
make_native_port_movie_audio_stream(const NativePortAudioConfig& config) {
    return std::unique_ptr<NativePortAudioStream>(
        new NativePortAudioStream(config,
                                  NativePortAudioStream::MovieTargetTag{}));
}

} // namespace katana::runtime
