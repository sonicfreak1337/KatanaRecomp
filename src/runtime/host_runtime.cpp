#include "katana/runtime/host_runtime.hpp"

#include "katana/io/json_report.hpp"
#include "katana/runtime/native_port_audio.hpp"
#include "native_port_audio_execution_domain.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifdef _MSC_VER
// Windows SDK 10.0.26100 annotates waveOutOpen callback parameters as value types.
#pragma warning(disable : 6553)
#endif
#define NOMINMAX
#include <windows.h>

#include <mmeapi.h>
#endif

namespace katana::runtime {
namespace {

[[nodiscard]] bool background_test_mode_requested() noexcept {
    static const bool requested = [] {
        const auto* const value = std::getenv("KATANA_PORT_BACKGROUND_TEST");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return requested;
}

void validate_audio(const std::span<const std::int16_t> samples, const std::uint32_t sample_rate) {
    if (samples.empty() || (samples.size() & 1u) != 0u || sample_rate == 0u) {
        throw std::invalid_argument("Host-Audio braucht vollstaendige Stereo-Frames.");
    }
}

std::uint64_t hash_audio(std::uint64_t hash,
                         const std::span<const std::int16_t> samples,
                         const std::uint32_t sample_rate) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    auto mix = [&](const std::uint8_t value) {
        hash ^= value;
        hash *= prime;
    };
    for (std::uint32_t shift = 0u; shift < 32u; shift += 8u)
        mix(static_cast<std::uint8_t>(sample_rate >> shift));
    for (const auto sample : samples) {
        const auto bits = static_cast<std::uint16_t>(sample);
        mix(static_cast<std::uint8_t>(bits));
        mix(static_cast<std::uint8_t>(bits >> 8u));
    }
    return hash;
}

std::uint64_t monotonic_now_ns() {
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (value < 0) throw std::runtime_error("Monotone Hostuhr lieferte einen negativen Wert.");
    return static_cast<std::uint64_t>(value);
}

void wait_until_ns(const std::uint64_t deadline) {
    const auto maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max());
    if (deadline > maximum) throw std::overflow_error("Host-Pacing-Deadline ist zu gross.");
    std::this_thread::sleep_until(
        std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
}

void wait_for_ns(const std::uint64_t duration) {
    const auto maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max());
    if (duration > maximum) throw std::overflow_error("Host-Wartezeit ist zu gross.");
    std::this_thread::sleep_for(std::chrono::nanoseconds(duration));
}

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

void saturating_add(std::uint64_t& value, const std::uint64_t increment_value) noexcept {
    if (increment_value > std::numeric_limits<std::uint64_t>::max() - value)
        value = std::numeric_limits<std::uint64_t>::max();
    else
        value += increment_value;
}

std::uint64_t required_wall_time_ns(const std::uint64_t thread_cpu_ns,
                                    const std::uint32_t target_cpu_percent) noexcept {
    const auto whole = thread_cpu_ns / target_cpu_percent;
    const auto remainder = thread_cpu_ns % target_cpu_percent;
    if (whole > std::numeric_limits<std::uint64_t>::max() / 100u)
        return std::numeric_limits<std::uint64_t>::max();
    auto result = whole * 100u;
    const auto fraction =
        (remainder * 100u + target_cpu_percent - 1u) / target_cpu_percent;
    if (fraction > std::numeric_limits<std::uint64_t>::max() - result)
        return std::numeric_limits<std::uint64_t>::max();
    result += fraction;
    return result;
}

std::uint64_t scaled_ratio_saturating(
    const std::uint64_t numerator,
    const std::uint64_t denominator,
    const std::uint64_t scale) noexcept {
    if (denominator == 0u || scale == 0u) return 0u;
    const auto whole = numerator / denominator;
    const auto remainder = numerator % denominator;
    if (whole > std::numeric_limits<std::uint64_t>::max() / scale)
        return std::numeric_limits<std::uint64_t>::max();
    auto result = whole * scale;
    std::uint64_t fraction = 0u;
    if (remainder <=
        std::numeric_limits<std::uint64_t>::max() / scale) {
        fraction = remainder * scale / denominator;
    } else {
        // Exact overflow-free floor(remainder * scale / denominator).
        // The telemetry scale is small (100000), and this slow branch is
        // reached only after many cumulative CPU hours.
        std::uint64_t reduced = 0u;
        for (std::uint64_t index = 0u; index < scale; ++index) {
            if (reduced >= denominator - remainder) {
                reduced -= denominator - remainder;
                ++fraction;
            } else {
                reduced += remainder;
            }
        }
    }
    if (fraction > std::numeric_limits<std::uint64_t>::max() - result)
        return std::numeric_limits<std::uint64_t>::max();
    return result + fraction;
}

#ifdef _WIN32
std::uint64_t process_cpu_now_ns() {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(
            GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
        throw std::runtime_error("Prozess-CPU-Zeit konnte nicht gelesen werden.");

    ULARGE_INTEGER kernel_ticks{};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER user_ticks{};
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    if (user_ticks.QuadPart >
        std::numeric_limits<std::uint64_t>::max() - kernel_ticks.QuadPart)
        throw std::overflow_error("Prozess-CPU-Zeit ist uebergelaufen.");
    const auto ticks = kernel_ticks.QuadPart + user_ticks.QuadPart;
    if (ticks > std::numeric_limits<std::uint64_t>::max() / 100u)
        throw std::overflow_error("Prozess-CPU-Zeit ist zu gross.");
    return ticks * 100u;
}
#elif defined(CLOCK_PROCESS_CPUTIME_ID)
std::uint64_t process_cpu_now_ns() {
    timespec value{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0)
        throw std::runtime_error("Prozess-CPU-Zeit konnte nicht gelesen werden.");
    if (value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1'000'000'000L)
        throw std::runtime_error("Prozess-CPU-Zeit ist ungueltig.");
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    if (seconds > std::numeric_limits<std::uint64_t>::max() / 1'000'000'000u)
        throw std::overflow_error("Prozess-CPU-Zeit ist zu gross.");
    return seconds * 1'000'000'000u +
           static_cast<std::uint64_t>(value.tv_nsec);
}
#else
std::uint64_t process_cpu_now_ns() {
    const auto ticks = std::clock();
    if (ticks == static_cast<std::clock_t>(-1) || ticks < 0)
        throw std::runtime_error("Prozess-CPU-Zeit konnte nicht gelesen werden.");
    const auto nanoseconds =
        static_cast<long double>(ticks) * 1'000'000'000.0L / CLOCKS_PER_SEC;
    if (nanoseconds >
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        throw std::overflow_error("Prozess-CPU-Zeit ist zu gross.");
    return static_cast<std::uint64_t>(nanoseconds);
}
#endif

#ifdef _WIN32
std::uint64_t thread_cpu_now_ns() {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) == FALSE)
        return process_cpu_now_ns();

    ULARGE_INTEGER kernel_ticks{};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER user_ticks{};
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    if (user_ticks.QuadPart >
        std::numeric_limits<std::uint64_t>::max() - kernel_ticks.QuadPart)
        throw std::overflow_error("Thread-CPU-Zeit ist uebergelaufen.");
    const auto ticks = kernel_ticks.QuadPart + user_ticks.QuadPart;
    if (ticks > std::numeric_limits<std::uint64_t>::max() / 100u)
        throw std::overflow_error("Thread-CPU-Zeit ist zu gross.");
    return ticks * 100u;
}
#elif defined(CLOCK_THREAD_CPUTIME_ID)
std::uint64_t thread_cpu_now_ns() {
    timespec value{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
        return process_cpu_now_ns();
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000L)
        throw std::runtime_error("Thread-CPU-Zeit ist ungueltig.");
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    if (seconds > std::numeric_limits<std::uint64_t>::max() / 1'000'000'000u)
        throw std::overflow_error("Thread-CPU-Zeit ist zu gross.");
    return seconds * 1'000'000'000u + static_cast<std::uint64_t>(value.tv_nsec);
}
#else
std::uint64_t thread_cpu_now_ns() {
    return process_cpu_now_ns();
}
#endif

#ifdef _WIN32
class Win32AudioOutput final : public HostAudioOutput {
  public:
    ~Win32AudioOutput() override {
        shutdown();
    }

    void submit(const std::span<const std::int16_t> samples,
                const std::uint32_t sample_rate) override {
        validate_audio(samples, sample_rate);
        if (shutdown_) throw std::logic_error("Host-Audio ist bereits heruntergefahren.");
        if (paused_) throw std::logic_error("Host-Audio akzeptiert im Pausezustand keinen Puffer.");
        if (samples.size_bytes() > std::numeric_limits<DWORD>::max())
            throw std::out_of_range("Host-Audiopuffer ist fuer WinMM zu gross.");
        open(sample_rate);
        reap();
        auto block = std::make_unique<Block>();
        block->samples.assign(samples.begin(), samples.end());
        if (background_test_mode_requested())
            std::ranges::fill(block->samples, 0);
        block->header.lpData = reinterpret_cast<LPSTR>(block->samples.data());
        block->header.dwBufferLength = static_cast<DWORD>(samples.size_bytes());
        if (waveOutPrepareHeader(device_, &block->header, sizeof(block->header)) !=
            MMSYSERR_NOERROR)
            throw std::runtime_error("WinMM-Audiopuffer konnte nicht vorbereitet werden.");
        block->prepared = true;
        if (waveOutWrite(device_, &block->header, sizeof(block->header)) != MMSYSERR_NOERROR) {
            static_cast<void>(
                waveOutUnprepareHeader(device_, &block->header, sizeof(block->header)));
            throw std::runtime_error("WinMM-Audiopuffer konnte nicht eingereiht werden.");
        }
        hash_ = hash_audio(hash_, samples, sample_rate);
        ++buffers_;
        frames_ += samples.size() / 2u;
        blocks_.push_back(std::move(block));
    }

    void bind_command_frame(const std::uint64_t frame_index) noexcept override {
        command_frame_index_ = std::max(command_frame_index_, frame_index);
    }

    void pause() override {
        if (shutdown_ || paused_) return;
        if (device_ != nullptr && waveOutPause(device_) != MMSYSERR_NOERROR)
            throw std::runtime_error("WinMM-Audio konnte nicht pausiert werden.");
        paused_ = true;
    }

    void resume() override {
        if (shutdown_) throw std::logic_error("Host-Audio ist bereits heruntergefahren.");
        if (!paused_) return;
        if (device_ != nullptr && waveOutRestart(device_) != MMSYSERR_NOERROR)
            throw std::runtime_error("WinMM-Audio konnte nicht fortgesetzt werden.");
        paused_ = false;
    }

    void shutdown() noexcept override {
        if (shutdown_) return;
        if (device_ != nullptr) {
            static_cast<void>(waveOutReset(device_));
            for (auto& block : blocks_) {
                if (block->prepared)
                    static_cast<void>(
                        waveOutUnprepareHeader(device_, &block->header, sizeof(block->header)));
            }
            blocks_.clear();
            static_cast<void>(waveOutClose(device_));
            device_ = nullptr;
        }
        paused_ = false;
        shutdown_ = true;
    }

    [[nodiscard]] bool paused() const noexcept override {
        return paused_;
    }
    [[nodiscard]] bool shutdown_complete() const noexcept override {
        return shutdown_;
    }
    [[nodiscard]] std::uint64_t submitted_buffers() const noexcept override {
        return buffers_;
    }
    [[nodiscard]] std::uint64_t submitted_frames() const noexcept override {
        return frames_;
    }
    [[nodiscard]] std::uint64_t deterministic_hash() const noexcept override {
        return hash_;
    }
    [[nodiscard]] NativePortAudioCommandQueueSnapshot
    command_queue_snapshot() const noexcept override {
        NativePortAudioCommandQueueSnapshot result;
        result.mode = NativePortAudioCommandQueueMode::SerialReference;
        result.lifecycle = shutdown_
                               ? NativePortAudioCommandQueueLifecycle::Stopped
                               : NativePortAudioCommandQueueLifecycle::Running;
        result.submitted_commands = buffers_;
        result.completed_commands = buffers_;
        result.last_submitted_stamp.frame_index = command_frame_index_;
        result.last_completed_stamp.frame_index = command_frame_index_;
        result.has_last_submitted_stamp = buffers_ != 0u;
        result.has_last_completed_stamp = buffers_ != 0u;
        return result;
    }

  private:
    struct Block {
        std::vector<std::int16_t> samples;
        WAVEHDR header{};
        bool prepared = false;
    };

    void open(const std::uint32_t sample_rate) {
        if (device_ != nullptr) {
            if (sample_rate != sample_rate_)
                throw std::invalid_argument(
                    "WinMM-Samplerate darf waehrend eines Laufs nicht wechseln.");
            return;
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2u;
        format.nSamplesPerSec = sample_rate;
        format.wBitsPerSample = 16u;
        format.nBlockAlign = 4u;
        format.nAvgBytesPerSec = sample_rate * format.nBlockAlign;
        if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0u, 0u, CALLBACK_NULL) != MMSYSERR_NOERROR)
            throw std::runtime_error("WinMM-Audioausgabe konnte nicht geoeffnet werden.");
        sample_rate_ = sample_rate;
    }

    void reap() noexcept {
        for (auto iterator = blocks_.begin(); iterator != blocks_.end();) {
            if (((*iterator)->header.dwFlags & WHDR_DONE) == 0u) {
                ++iterator;
                continue;
            }
            static_cast<void>(
                waveOutUnprepareHeader(device_, &(*iterator)->header, sizeof((*iterator)->header)));
            iterator = blocks_.erase(iterator);
        }
    }

    HWAVEOUT device_ = nullptr;
    std::vector<std::unique_ptr<Block>> blocks_;
    std::uint32_t sample_rate_ = 0u;
    std::uint64_t hash_ = 1469598103934665603ull;
    std::uint64_t buffers_ = 0u;
    std::uint64_t frames_ = 0u;
    std::uint64_t command_frame_index_ = 0u;
    bool paused_ = false;
    bool shutdown_ = false;
};

enum class HostAudioCommandOpcode : std::uint16_t {
    Construct = 1u,
    Submit,
    Pause,
    Resume,
    Destroy,
};

struct HostAudioSubmitCommand final {
    std::uint32_t sample_rate = 0u;
    std::uint32_t sample_count = 0u;
};

static_assert(std::is_trivially_copyable_v<HostAudioSubmitCommand>);

class DomainHostAudioOutput final : public HostAudioOutput {
  public:
    explicit DomainHostAudioOutput(NativePortTelemetry* const telemetry)
        : producer_thread_(std::this_thread::get_id()),
          domain_(acquire_native_port_audio_execution_domain()),
          telemetry_(telemetry) {
        if (telemetry_ != nullptr) {
            if (!domain_->bind_telemetry(telemetry_))
                throw std::runtime_error(
                    "Audio-Domain-Telemetrie konnte nicht gebunden werden.");
            telemetry_bound_ = true;
        }
        const auto registered = domain_->register_target(
            NativePortAudioExecutionDomainTarget::HostOutput,
            this,
            &DomainHostAudioOutput::execute_worker_command,
            &DomainHostAudioOutput::cleanup_worker_state);
        if (!registered.has_value()) {
            release_telemetry();
            throw std::runtime_error("Host-Audio-Domainziel konnte nicht registriert werden.");
        }
        handle_ = *registered;
        const auto result = domain_->dispatch_sync(
            handle_,
            static_cast<std::uint16_t>(HostAudioCommandOpcode::Construct),
            {},
            0u);
        if (!result.completed()) {
            // Construct may have installed worker-owned output state before
            // returning a failed ACK.  Keep terminal cleanup armed and join
            // the sole consumer before this facade can unwind.
            domain_->shutdown();
            handle_ = {};
            release_telemetry();
            throw std::runtime_error("Host-Audio-Domainziel konnte nicht konstruiert werden.");
        }
    }

    ~DomainHostAudioOutput() override { shutdown(); }

    void submit(const std::span<const std::int16_t> samples,
                const std::uint32_t sample_rate) override {
        require_producer_thread();
        require_accepting();
        validate_audio(samples, sample_rate);
        if (samples.size() > std::numeric_limits<std::uint32_t>::max() ||
            samples.size_bytes() >
                std::numeric_limits<std::uint32_t>::max() -
                    sizeof(HostAudioSubmitCommand))
            throw std::out_of_range("Host-Audiopuffer ist zu gross.");
        const HostAudioSubmitCommand command{
            sample_rate, static_cast<std::uint32_t>(samples.size())};
        const std::array parts{
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(&command),
                static_cast<std::uint32_t>(sizeof(command))},
            NativePortAudioExecutionDomainPayloadPart{
                reinterpret_cast<const std::byte*>(samples.data()),
                static_cast<std::uint32_t>(samples.size_bytes())}};
        const auto result = domain_->dispatch_async_scatter(
            handle_,
            static_cast<std::uint16_t>(HostAudioCommandOpcode::Submit),
            parts,
            current_frame_index());
        require_dispatch(result, "submit");
    }

    void render_and_submit_aica(
        AicaRegisterFile& aica,
        const std::size_t frame_count,
        const std::uint32_t sample_rate) override {
        require_producer_thread();
        require_accepting();
        if (sample_rate == 0u ||
            frame_count > std::numeric_limits<std::uint32_t>::max() / 2u)
            throw std::invalid_argument(
                "AICA-Audioausgabe besitzt eine ungueltige Geometrie.");
        const auto sample_count = static_cast<std::uint32_t>(frame_count * 2u);
        const auto sample_bytes =
            static_cast<std::uint64_t>(sample_count) * sizeof(std::int16_t);
        if (sample_bytes >
            std::numeric_limits<std::uint32_t>::max() -
                sizeof(HostAudioSubmitCommand))
            throw std::out_of_range("AICA-Audiopuffer ist zu gross.");
        const auto payload_size = static_cast<std::uint32_t>(
            sizeof(HostAudioSubmitCommand) + sample_bytes);
        auto lease = domain_->begin_async_payload(
            handle_,
            static_cast<std::uint16_t>(HostAudioCommandOpcode::Submit),
            payload_size,
            current_frame_index());
        if (!lease.valid())
            throw std::runtime_error("Host-Audioqueue konnte AICA-PCM nicht reservieren.");
        const HostAudioSubmitCommand command{sample_rate, sample_count};
        auto payload = lease.payload();
        std::memcpy(payload.data(), &command, sizeof(command));
        auto* const samples = reinterpret_cast<std::int16_t*>(
            payload.data() + sizeof(command));
        try {
            aica.render_audio_into(
                std::span<std::int16_t>(samples, sample_count), sample_rate);
        } catch (...) {
            lease.abort();
            throw;
        }
        if (!lease.publish())
            throw std::runtime_error("Host-Audioqueue konnte AICA-PCM nicht publizieren.");
    }

    void bind_command_frame(const std::uint64_t frame_index) noexcept override {
        auto current = bound_frame_index_.load(std::memory_order_acquire);
        while (current < frame_index &&
               !bound_frame_index_.compare_exchange_weak(
                   current,
                   frame_index,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
    }

    void pause() override {
        require_producer_thread();
        if (shutdown_requested_.load(std::memory_order_acquire) ||
            paused_.load(std::memory_order_acquire))
            return;
        require_dispatch(
            domain_->dispatch_sync(
                handle_,
                static_cast<std::uint16_t>(HostAudioCommandOpcode::Pause),
                {},
                current_frame_index()),
            "pause");
    }

    void resume() override {
        require_producer_thread();
        if (shutdown_requested_.load(std::memory_order_acquire))
            throw std::logic_error("Host-Audio ist bereits heruntergefahren.");
        if (!paused_.load(std::memory_order_acquire)) return;
        require_dispatch(
            domain_->dispatch_sync(
                handle_,
                static_cast<std::uint16_t>(HostAudioCommandOpcode::Resume),
                {},
                current_frame_index()),
            "resume");
    }

    void shutdown() noexcept override {
        if (shutdown_requested_.exchange(true, std::memory_order_acq_rel))
            return;
        bool destroyed = false;
        if (domain_ != nullptr && handle_.valid()) {
            const auto result = domain_->dispatch_sync(
                handle_,
                static_cast<std::uint16_t>(HostAudioCommandOpcode::Destroy),
                {},
                current_frame_index());
            destroyed = result.completed();
            if (destroyed &&
                !domain_->unregister_target(handle_, this))
                destroyed = false;
        }
        if (!destroyed && domain_ != nullptr) {
            // A terminal domain failure cancels every queued command before
            // target storage is released. Do not leave a live worker with a
            // dangling facade identity.
            domain_->shutdown();
        }
        handle_ = {};
        release_telemetry();
        domain_.reset();
        paused_.store(false, std::memory_order_release);
        shutdown_complete_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool paused() const noexcept override {
        return paused_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool shutdown_complete() const noexcept override {
        return shutdown_complete_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t submitted_buffers() const noexcept override {
        return submitted_buffers_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t submitted_frames() const noexcept override {
        return submitted_frames_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t deterministic_hash() const noexcept override {
        return deterministic_hash_.load(std::memory_order_acquire);
    }

    [[nodiscard]] NativePortAudioCommandQueueSnapshot
    command_queue_snapshot() const noexcept override {
        return domain_ != nullptr ? domain_->snapshot().queue
                                  : NativePortAudioCommandQueueSnapshot{};
    }

  private:
    void release_telemetry() noexcept {
        if (!telemetry_bound_ || domain_ == nullptr || telemetry_ == nullptr)
            return;
        if (!domain_->unbind_telemetry(telemetry_)) domain_->shutdown();
        telemetry_bound_ = false;
    }

    static void execute_worker_command(
        void* const target,
        const std::uint16_t raw_opcode,
        const std::span<const std::byte> payload,
        NativePortAudioCommandAckResult& result) noexcept {
        result = {};
        auto* const self = static_cast<DomainHostAudioOutput*>(target);
        if (self == nullptr) {
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 1u;
            return;
        }
        try {
            self->execute(
                static_cast<HostAudioCommandOpcode>(raw_opcode),
                payload);
        } catch (...) {
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 1u;
        }
    }

    static void cleanup_worker_state(void* const target) noexcept {
        auto& self = *static_cast<DomainHostAudioOutput*>(target);
        self.output_.reset();
        self.worker_constructed_ = false;
        self.paused_.store(false, std::memory_order_release);
    }

    void execute(const HostAudioCommandOpcode opcode,
                 const std::span<const std::byte> payload) {
        switch (opcode) {
        case HostAudioCommandOpcode::Construct:
            require_empty(payload);
            if (worker_constructed_)
                throw std::logic_error("Host-Audio wurde doppelt konstruiert.");
            worker_constructed_ = true;
            return;
        case HostAudioCommandOpcode::Destroy:
            require_empty(payload);
            if (output_ != nullptr) {
                output_->stop();
                output_.reset();
            }
            worker_constructed_ = false;
            paused_.store(false, std::memory_order_release);
            return;
        case HostAudioCommandOpcode::Submit:
            execute_submit(payload);
            return;
        case HostAudioCommandOpcode::Pause:
            require_empty(payload);
            require_worker_constructed();
            if (output_ != nullptr) output_->pause();
            paused_.store(true, std::memory_order_release);
            return;
        case HostAudioCommandOpcode::Resume:
            require_empty(payload);
            require_worker_constructed();
            if (output_ != nullptr) output_->resume();
            paused_.store(false, std::memory_order_release);
            return;
        }
        throw std::invalid_argument("Host-Audio Opcode.");
    }

    void execute_submit(const std::span<const std::byte> payload) {
        require_worker_constructed();
        if (payload.size() < sizeof(HostAudioSubmitCommand))
            throw std::invalid_argument("Host-Audio Submit-Payload.");
        HostAudioSubmitCommand command;
        std::memcpy(&command, payload.data(), sizeof(command));
        const auto sample_bytes =
            static_cast<std::uint64_t>(command.sample_count) *
            sizeof(std::int16_t);
        if (command.sample_count == 0u || (command.sample_count & 1u) != 0u ||
            sample_bytes > std::numeric_limits<std::size_t>::max() ||
            sizeof(command) + sample_bytes != payload.size())
            throw std::invalid_argument("Host-Audio Submit-Geometrie.");
        const auto* const samples = reinterpret_cast<const std::int16_t*>(
            payload.data() + sizeof(command));
        const std::span<const std::int16_t> pcm(
            samples, command.sample_count);
        validate_audio(pcm, command.sample_rate);
        if (output_ == nullptr) {
            sample_rate_ = command.sample_rate;
            output_ = std::make_unique<NativePortAudioStream>(
                NativePortAudioConfig{{sample_rate_, 2u}, 44'100u});
        } else if (sample_rate_ != command.sample_rate) {
            throw std::invalid_argument(
                "Host-Audio Samplerate darf nicht wechseln.");
        }
        while (!output_->submit_pcm_s16(pcm)) {
            output_->poll();
            std::this_thread::yield();
        }
        deterministic_hash_.store(
            hash_audio(
                deterministic_hash_.load(std::memory_order_relaxed),
                pcm,
                command.sample_rate),
            std::memory_order_release);
        submitted_buffers_.fetch_add(1u, std::memory_order_release);
        submitted_frames_.fetch_add(
            pcm.size() / 2u, std::memory_order_release);
    }

    void require_producer_thread() const {
        if (std::this_thread::get_id() != producer_thread_)
            throw std::logic_error("Host-Audio Producer-Thread verletzt.");
    }

    void require_accepting() const {
        if (shutdown_requested_.load(std::memory_order_acquire))
            throw std::logic_error("Host-Audio ist bereits heruntergefahren.");
        if (paused_.load(std::memory_order_acquire))
            throw std::logic_error(
                "Host-Audio akzeptiert im Pausezustand keinen Puffer.");
    }

    void require_worker_constructed() const {
        if (!worker_constructed_)
            throw std::logic_error("Host-Audio Worker ist nicht konstruiert.");
    }

    static void require_empty(const std::span<const std::byte> payload) {
        if (!payload.empty())
            throw std::invalid_argument("Host-Audio Lifecycle-Payload.");
    }

    static void require_dispatch(
        const NativePortAudioExecutionDomainDispatchResult& result,
        const char* const operation) {
        if (result.completed()) return;
        throw std::runtime_error(
            std::string("Host-Audio-Domainfehler:") + operation + ":" +
            std::to_string(static_cast<unsigned>(result.failure)));
    }

    [[nodiscard]] std::uint64_t current_frame_index() const noexcept {
        auto frame = bound_frame_index_.load(std::memory_order_acquire);
        if (const auto media = current_media_audio_tick_evidence();
            media.has_value())
            frame = std::max(frame, media->frame_index);
        const auto domain_snapshot = domain_->snapshot();
        if (domain_snapshot.has_last_frame_index)
            frame = std::max(frame, domain_snapshot.last_frame_index);
        return frame;
    }

    std::thread::id producer_thread_;
    std::shared_ptr<NativePortAudioExecutionDomain> domain_;
    NativePortAudioExecutionDomainTargetHandle handle_{};
    std::unique_ptr<NativePortAudioStream> output_;
    NativePortTelemetry* telemetry_ = nullptr;
    bool telemetry_bound_ = false;
    std::atomic<std::uint64_t> bound_frame_index_{0u};
    std::atomic<std::uint64_t> submitted_buffers_{0u};
    std::atomic<std::uint64_t> submitted_frames_{0u};
    std::atomic<std::uint64_t> deterministic_hash_{1469598103934665603ull};
    std::atomic<bool> paused_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> shutdown_complete_{false};
    std::uint32_t sample_rate_ = 0u;
    bool worker_constructed_ = false;
};
#endif

} // namespace

HostPacingException::HostPacingException(const HostPacingError error,
                                         const std::uint64_t guest_cycle)
    : std::runtime_error("Host-Pacing-Fehler " + std::string(host_pacing_error_name(error)) +
                         " bei Gastzyklus " + std::to_string(guest_cycle) + '.'),
      error_(error), guest_cycle_(guest_cycle) {}

HostPacingError HostPacingException::error() const noexcept {
    return error_;
}
std::uint64_t HostPacingException::guest_cycle() const noexcept {
    return guest_cycle_;
}

std::string HostPacingException::serialize_json() const {
    std::ostringstream output;
    katana::io::write_json_report_header(
        output, "katana-host-pacing-error-v1", "host-pacing-error");
    output << ",\"contract_version\":" << host_pacing_contract_version
           << ",\"error\":" << katana::io::quote_json(host_pacing_error_name(error_))
           << ",\"guest_cycle\":" << guest_cycle_ << '}';
    return output.str();
}

HostPacer::HostPacer(const HostPacingConfig config, HostMonotonicNow now, HostWaitUntil wait_until)
    : config_(config), now_(std::move(now)), wait_until_(std::move(wait_until)) {
    constexpr auto nanoseconds_per_second = 1'000'000'000ull;
    if (config_.guest_cycles_per_second == 0u ||
        config_.guest_cycles_per_second >
            std::numeric_limits<std::uint64_t>::max() / nanoseconds_per_second)
        throw std::invalid_argument("Host-Pacing besitzt eine ungueltige Gastzyklusrate.");
    if (!now_) now_ = monotonic_now_ns;
    if (!wait_until_) wait_until_ = wait_until_ns;
}

void HostPacer::resume(const std::uint64_t guest_cycle) {
    if (shutdown_) throw std::logic_error("Host-Pacing ist bereits heruntergefahren.");
    const auto now = now_();
    if (initialized_ && (guest_cycle < last_guest_cycle_ || now < last_host_ns_))
        fail(guest_cycle < last_guest_cycle_ ? HostPacingError::GuestCycleRegression
                                             : HostPacingError::HostClockRegression,
             guest_cycle);
    anchor_guest_cycle_ = guest_cycle;
    last_guest_cycle_ = guest_cycle;
    anchor_host_ns_ = now;
    last_host_ns_ = now;
    initialized_ = true;
    running_ = true;
}

void HostPacer::pause(const std::uint64_t guest_cycle) {
    if (shutdown_ || !running_) return;
    if (guest_cycle < last_guest_cycle_) fail(HostPacingError::GuestCycleRegression, guest_cycle);
    const auto now = now_();
    if (now < last_host_ns_) fail(HostPacingError::HostClockRegression, guest_cycle);
    last_guest_cycle_ = guest_cycle;
    last_host_ns_ = now;
    running_ = false;
}

void HostPacer::pace(const std::uint64_t guest_cycle) {
    if (shutdown_ || !running_) return;
    if (guest_cycle < last_guest_cycle_ || guest_cycle < anchor_guest_cycle_)
        fail(HostPacingError::GuestCycleRegression, guest_cycle);
    constexpr auto nanoseconds_per_second = 1'000'000'000ull;
    const auto guest_delta = guest_cycle - anchor_guest_cycle_;
    const auto whole_seconds = guest_delta / config_.guest_cycles_per_second;
    const auto remainder = guest_delta % config_.guest_cycles_per_second;
    if (whole_seconds > std::numeric_limits<std::uint64_t>::max() / nanoseconds_per_second)
        fail(HostPacingError::DeadlineOverflow, guest_cycle);
    const auto whole_nanoseconds = whole_seconds * nanoseconds_per_second;
    const auto fractional_nanoseconds =
        (remainder * nanoseconds_per_second) / config_.guest_cycles_per_second;
    if (whole_nanoseconds > std::numeric_limits<std::uint64_t>::max() - fractional_nanoseconds ||
        anchor_host_ns_ >
            std::numeric_limits<std::uint64_t>::max() - whole_nanoseconds - fractional_nanoseconds)
        fail(HostPacingError::DeadlineOverflow, guest_cycle);
    const auto deadline = anchor_host_ns_ + whole_nanoseconds + fractional_nanoseconds;
    if (deadline >
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max()))
        fail(HostPacingError::DeadlineOverflow, guest_cycle);
    auto now = now_();
    if (now < last_host_ns_) fail(HostPacingError::HostClockRegression, guest_cycle);
    if (now < deadline) {
        wait_until_(deadline);
        increment(wait_calls_);
        now = now_();
        if (now < deadline) fail(HostPacingError::WaitReturnedEarly, guest_cycle);
    } else if (now > deadline) {
        increment(late_deadlines_);
    }
    last_guest_cycle_ = guest_cycle;
    last_host_ns_ = now;
}

void HostPacer::shutdown() noexcept {
    running_ = false;
    shutdown_ = true;
}
bool HostPacer::running() const noexcept {
    return running_;
}
bool HostPacer::shutdown_complete() const noexcept {
    return shutdown_;
}
std::uint64_t HostPacer::wait_calls() const noexcept {
    return wait_calls_;
}
std::uint64_t HostPacer::late_deadlines() const noexcept {
    return late_deadlines_;
}
std::uint64_t HostPacer::last_guest_cycle() const noexcept {
    return last_guest_cycle_;
}
const std::optional<HostPacingFirstError>& HostPacer::first_error() const noexcept {
    return first_error_;
}

[[noreturn]] void HostPacer::fail(const HostPacingError error, const std::uint64_t guest_cycle) {
    if (!first_error_) first_error_ = HostPacingFirstError{error, guest_cycle};
    running_ = false;
    throw HostPacingException(error, guest_cycle);
}

std::string HostPacer::serialize_status_json() const {
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-host-pacing-v1", "host-pacing");
    output << ",\"contract_version\":" << host_pacing_contract_version
           << ",\"running\":" << (running_ ? "true" : "false")
           << ",\"shutdown\":" << (shutdown_ ? "true" : "false")
           << ",\"wait_calls\":" << wait_calls_ << ",\"late_deadlines\":" << late_deadlines_
           << ",\"last_guest_cycle\":" << last_guest_cycle_ << ",\"first_error\":";
    if (first_error_)
        output << "{\"error\":"
               << katana::io::quote_json(host_pacing_error_name(first_error_->error))
               << ",\"guest_cycle\":" << first_error_->guest_cycle << '}';
    else
        output << "null";
    output << '}';
    return output.str();
}

const char* host_pacing_error_name(const HostPacingError value) noexcept {
    switch (value) {
    case HostPacingError::None:
        return "none";
    case HostPacingError::GuestCycleRegression:
        return "guest-cycle-regression";
    case HostPacingError::HostClockRegression:
        return "host-clock-regression";
    case HostPacingError::DeadlineOverflow:
        return "deadline-overflow";
    case HostPacingError::WaitReturnedEarly:
        return "wait-returned-early";
    }
    return "none";
}

HostWorkloadLimiter::HostWorkloadLimiter(const HostWorkloadLimiterConfig config,
                                         HostMonotonicNow wall_now,
                                         HostThreadCpuNow thread_cpu_now,
                                         HostWaitFor wait_for,
                                         HostProcessCpuNow process_cpu_now)
    : config_(config), wall_now_(std::move(wall_now)),
      thread_cpu_now_(std::move(thread_cpu_now)),
      wait_for_(std::move(wait_for)),
      process_cpu_now_(std::move(process_cpu_now)) {
    if (config_.target_cpu_percent == 0u || config_.target_cpu_percent > 100u)
        throw std::invalid_argument("Host-Lastbegrenzung besitzt eine ungueltige Zielquote.");
    if (config_.target_process_cpu_percent == 0u ||
        config_.target_process_cpu_percent > 100u ||
        config_.process_cpu_capacity == 0u ||
        config_.process_cpu_capacity >
            std::numeric_limits<std::uint32_t>::max() / 100u)
        throw std::invalid_argument(
            "Host-Lastbegrenzung besitzt eine ungueltige Prozessquote.");
    if (!enabled()) return;
    if (config_.accounting_window_ns == 0u)
        throw std::invalid_argument(
            "Host-Lastbegrenzung besitzt kein Abrechnungsfenster.");
    if (config_.maximum_wait_ns == 0u ||
        config_.maximum_wait_ns > host_workload_limiter_maximum_wait_ceiling_ns)
        throw std::invalid_argument(
            "Host-Lastbegrenzung besitzt ein ungueltiges Warteintervall.");
    if (!wall_now_) wall_now_ = monotonic_now_ns;
    if (config_.target_cpu_percent < 100u && !thread_cpu_now_)
        thread_cpu_now_ = thread_cpu_now_ns;
    if (config_.target_process_cpu_percent < 100u && !process_cpu_now_)
        process_cpu_now_ = process_cpu_now_ns;
    if (!wait_for_) wait_for_ = wait_for_ns;
}

void HostWorkloadLimiter::limit() {
    if (!enabled()) return;
    limit_at(wall_now_());
}

void HostWorkloadLimiter::limit_if_due() {
    if (!enabled()) return;
    const auto wall_now = wall_now_();
    if (initialized_ && wall_now >= last_wall_ns_ &&
        wall_now - last_wall_ns_ < host_workload_limiter_safepoint_interval_ns)
        return;
    limit_at(wall_now);
}

void HostWorkloadLimiter::limit_at(std::uint64_t wall_now) {
    auto thread_cpu_now = config_.target_cpu_percent < 100u
        ? thread_cpu_now_()
        : 0u;
    auto process_cpu_now = config_.target_process_cpu_percent < 100u
        ? process_cpu_now_()
        : 0u;
    if (!initialized_) {
        rebase(wall_now, thread_cpu_now, process_cpu_now);
        return;
    }
    if (wall_now < last_wall_ns_ ||
        thread_cpu_now < last_thread_cpu_ns_ ||
        process_cpu_now < last_process_cpu_ns_) {
        rebase(wall_now, thread_cpu_now, process_cpu_now);
        return;
    }
    observe_sample(wall_now, thread_cpu_now, process_cpu_now);

    for (;;) {
        const auto wall_elapsed = wall_now - anchor_wall_ns_;
        const auto thread_cpu_elapsed =
            thread_cpu_now - anchor_thread_cpu_ns_;
        auto required_wall = std::uint64_t{0u};
        if (config_.target_cpu_percent < 100u)
            required_wall = required_wall_time_ns(
                thread_cpu_elapsed, config_.target_cpu_percent);
        if (config_.target_process_cpu_percent < 100u) {
            const auto aggregate_process_target =
                config_.target_process_cpu_percent *
                config_.process_cpu_capacity;
            required_wall = std::max(
                required_wall,
                required_wall_time_ns(
                    process_cpu_now - anchor_process_cpu_ns_,
                    aggregate_process_target));
        }
        if (required_wall == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error(
                "Host-Lastbegrenzungsschuld ist nicht darstellbar.");
        if (required_wall <= wall_elapsed) {
            if (wall_elapsed >= config_.accounting_window_ns)
                rebase(wall_now, thread_cpu_now, process_cpu_now);
            return;
        }
        const auto wait_duration =
            std::min(required_wall - wall_elapsed, config_.maximum_wait_ns);
        wait_for_(wait_duration);
        increment(wait_calls_);

        const auto wall_after_wait = wall_now_();
        const auto thread_cpu_after_wait =
            config_.target_cpu_percent < 100u ? thread_cpu_now_() : 0u;
        const auto process_cpu_after_wait =
            config_.target_process_cpu_percent < 100u
                ? process_cpu_now_()
                : 0u;
        if (wall_after_wait >= wall_now)
            saturating_add(wait_time_ns_, wall_after_wait - wall_now);
        if (wall_after_wait < wall_now ||
            thread_cpu_after_wait < thread_cpu_now ||
            process_cpu_after_wait < process_cpu_now) {
            rebase(
                wall_after_wait, thread_cpu_after_wait, process_cpu_after_wait);
            return;
        }
        if (wall_after_wait == wall_now)
            throw std::runtime_error(
                "Host-Lastbegrenzungswait erzeugte keinen Zeitfortschritt.");
        wall_now = wall_after_wait;
        thread_cpu_now = thread_cpu_after_wait;
        process_cpu_now = process_cpu_after_wait;
        observe_sample(wall_now, thread_cpu_now, process_cpu_now);
    }
}

void HostWorkloadLimiter::reset() noexcept {
    anchor_wall_ns_ = 0u;
    anchor_thread_cpu_ns_ = 0u;
    anchor_process_cpu_ns_ = 0u;
    last_wall_ns_ = 0u;
    last_thread_cpu_ns_ = 0u;
    last_process_cpu_ns_ = 0u;
    initialized_ = false;
}

bool HostWorkloadLimiter::enabled() const noexcept {
    return config_.target_cpu_percent < 100u ||
           config_.target_process_cpu_percent < 100u;
}

bool HostWorkloadLimiter::initialized() const noexcept {
    return initialized_;
}

std::uint32_t HostWorkloadLimiter::target_cpu_percent() const noexcept {
    return config_.target_cpu_percent;
}

std::uint32_t
HostWorkloadLimiter::target_process_cpu_percent() const noexcept {
    return config_.target_process_cpu_percent;
}

std::uint32_t HostWorkloadLimiter::process_cpu_capacity() const noexcept {
    return config_.process_cpu_capacity;
}

std::uint64_t HostWorkloadLimiter::wait_calls() const noexcept {
    return wait_calls_;
}

std::uint64_t HostWorkloadLimiter::wait_time_ns() const noexcept {
    return wait_time_ns_;
}

std::uint64_t HostWorkloadLimiter::measured_wall_time_ns() const noexcept {
    return measured_wall_time_ns_;
}

std::uint64_t
HostWorkloadLimiter::measured_thread_cpu_time_ns() const noexcept {
    return measured_thread_cpu_time_ns_;
}

std::uint64_t
HostWorkloadLimiter::measured_process_cpu_time_ns() const noexcept {
    return measured_process_cpu_time_ns_;
}

std::uint64_t
HostWorkloadLimiter::measured_cpu_percent_milli() const noexcept {
    constexpr std::uint64_t scale = 100'000u;
    return scaled_ratio_saturating(
        measured_thread_cpu_time_ns_,
        measured_wall_time_ns_,
        scale);
}

std::uint64_t
HostWorkloadLimiter::measured_process_cpu_percent_milli() const noexcept {
    constexpr std::uint64_t scale = 100'000u;
    return scaled_ratio_saturating(
        measured_process_cpu_time_ns_,
        measured_wall_time_ns_,
        scale);
}

std::string HostWorkloadLimiter::serialize_status_json() const {
    std::ostringstream output;
    katana::io::write_json_report_header(
        output, "katana-host-workload-limiter-v2", "host-workload-limiter");
    output << ",\"contract_version\":" << host_workload_limiter_contract_version
           << ",\"enabled\":" << (enabled() ? "true" : "false")
           << ",\"initialized\":" << (initialized_ ? "true" : "false")
           << ",\"target_cpu_percent\":" << config_.target_cpu_percent
           << ",\"target_process_cpu_percent\":"
           << config_.target_process_cpu_percent
           << ",\"process_cpu_capacity\":" << config_.process_cpu_capacity
           << ",\"accounting_window_ns\":" << config_.accounting_window_ns
           << ",\"maximum_wait_ns\":" << config_.maximum_wait_ns
           << ",\"wait_calls\":" << wait_calls_
           << ",\"wait_time_ns\":" << wait_time_ns_
           << ",\"measured_wall_time_ns\":" << measured_wall_time_ns_
           << ",\"measured_thread_cpu_time_ns\":"
           << measured_thread_cpu_time_ns_
           << ",\"measured_cpu_percent_milli\":"
           << measured_cpu_percent_milli()
           << ",\"measured_process_cpu_time_ns\":"
           << measured_process_cpu_time_ns_
           << ",\"measured_process_cpu_percent_milli\":"
           << measured_process_cpu_percent_milli() << '}';
    return output.str();
}

void HostWorkloadLimiter::observe_sample(
    const std::uint64_t wall_ns,
    const std::uint64_t thread_cpu_ns,
    const std::uint64_t process_cpu_ns) noexcept {
    saturating_add(measured_wall_time_ns_, wall_ns - last_wall_ns_);
    saturating_add(
        measured_thread_cpu_time_ns_, thread_cpu_ns - last_thread_cpu_ns_);
    saturating_add(
        measured_process_cpu_time_ns_,
        process_cpu_ns - last_process_cpu_ns_);
    last_wall_ns_ = wall_ns;
    last_thread_cpu_ns_ = thread_cpu_ns;
    last_process_cpu_ns_ = process_cpu_ns;
}

void HostWorkloadLimiter::rebase(const std::uint64_t wall_ns,
                                 const std::uint64_t thread_cpu_ns,
                                 const std::uint64_t process_cpu_ns) noexcept {
    anchor_wall_ns_ = wall_ns;
    anchor_thread_cpu_ns_ = thread_cpu_ns;
    anchor_process_cpu_ns_ = process_cpu_ns;
    last_wall_ns_ = wall_ns;
    last_thread_cpu_ns_ = thread_cpu_ns;
    last_process_cpu_ns_ = process_cpu_ns;
    initialized_ = true;
}

void HostAudioOutput::render_and_submit_aica(
    AicaRegisterFile& aica,
    const std::size_t frame_count,
    const std::uint32_t sample_rate) {
    if (sample_rate == 0u ||
        frame_count > std::numeric_limits<std::size_t>::max() / 2u)
        throw std::invalid_argument(
            "AICA-Audioausgabe besitzt eine ungueltige Geometrie.");
    // This fallback is intentionally limited to Recording/SerialReference.
    // The dedicated implementation overrides the method and reserves its
    // owning domain payload before AICA writes a single sample.
    static thread_local std::vector<std::int16_t> serial_samples;
    serial_samples.resize(frame_count * 2u);
    aica.render_audio_into(serial_samples, sample_rate);
    submit(serial_samples, sample_rate);
}

void RecordingHostAudioOutput::submit(const std::span<const std::int16_t> samples,
                                      const std::uint32_t sample_rate) {
    validate_audio(samples, sample_rate);
    if (shutdown_) throw std::logic_error("Recording-Audio ist bereits heruntergefahren.");
    if (paused_) throw std::logic_error("Recording-Audio akzeptiert in Pause keinen Puffer.");
    hash_ = hash_audio(hash_, samples, sample_rate);
    ++buffers_;
    frames_ += samples.size() / 2u;
}
void RecordingHostAudioOutput::bind_command_frame(
    const std::uint64_t frame_index) noexcept {
    command_frame_index_ = std::max(command_frame_index_, frame_index);
}
void RecordingHostAudioOutput::pause() {
    if (!shutdown_) paused_ = true;
}
void RecordingHostAudioOutput::resume() {
    if (shutdown_) throw std::logic_error("Recording-Audio ist bereits heruntergefahren.");
    paused_ = false;
}
void RecordingHostAudioOutput::shutdown() noexcept {
    paused_ = false;
    shutdown_ = true;
}
bool RecordingHostAudioOutput::paused() const noexcept {
    return paused_;
}
bool RecordingHostAudioOutput::shutdown_complete() const noexcept {
    return shutdown_;
}
std::uint64_t RecordingHostAudioOutput::submitted_buffers() const noexcept {
    return buffers_;
}
std::uint64_t RecordingHostAudioOutput::submitted_frames() const noexcept {
    return frames_;
}
std::uint64_t RecordingHostAudioOutput::deterministic_hash() const noexcept {
    return hash_;
}
NativePortAudioCommandQueueSnapshot
RecordingHostAudioOutput::command_queue_snapshot() const noexcept {
    NativePortAudioCommandQueueSnapshot result;
    result.mode = NativePortAudioCommandQueueMode::SerialReference;
    result.lifecycle = shutdown_ ? NativePortAudioCommandQueueLifecycle::Stopped
                                 : NativePortAudioCommandQueueLifecycle::Running;
    result.submitted_commands = buffers_;
    result.completed_commands = buffers_;
    result.last_submitted_stamp.frame_index = command_frame_index_;
    result.last_completed_stamp.frame_index = command_frame_index_;
    result.has_last_submitted_stamp = buffers_ != 0u;
    result.has_last_completed_stamp = buffers_ != 0u;
    return result;
}

bool native_audio_available() noexcept {
#ifdef _WIN32
    return waveOutGetNumDevs() != 0u;
#else
    return false;
#endif
}

std::unique_ptr<HostAudioOutput> create_native_audio_output(
    NativePortTelemetry* const telemetry) {
#ifdef _WIN32
    if (native_port_audio_serial_reference_requested())
        return std::make_unique<Win32AudioOutput>();
    return std::make_unique<DomainHostAudioOutput>(telemetry);
#else
    static_cast<void>(telemetry);
    throw std::runtime_error("Native Audioausgabe ist auf diesem Host nicht verfuegbar.");
#endif
}

void InjectedHostInput::inject(const std::uint64_t sequence,
                               const std::uint64_t guest_cycle,
                               const ControllerState state) {
    if (sequence == 0u || sequence <= last_sequence_ || guest_cycle < last_guest_cycle_)
        throw std::invalid_argument(
            "Hosteingabe ist nicht streng sequenziert oder gastzeitmonoton.");
    state_ = state;
    last_sequence_ = sequence;
    last_guest_cycle_ = guest_cycle;
    ++injected_events_;
}
ControllerState InjectedHostInput::sample(const std::uint64_t) {
    ++sampled_frames_;
    return state_;
}
std::uint64_t InjectedHostInput::injected_events() const noexcept {
    return injected_events_;
}
std::uint64_t InjectedHostInput::sampled_frames() const noexcept {
    return sampled_frames_;
}

HostRuntimeSession::HostRuntimeSession(EventScheduler& scheduler,
                                       DreamcastMediaClock& media_clock,
                                       std::shared_ptr<InjectedHostInput> input,
                                       HostAudioOutput& audio,
                                       HostPacer* pacer,
                                       HostShutdownCallback shutdown_callback)
    : scheduler_(scheduler), media_clock_(media_clock), input_(std::move(input)), audio_(audio),
      pacer_(pacer), shutdown_callback_(std::move(shutdown_callback)) {
    if (!input_) throw std::invalid_argument("Hostruntime braucht ein Eingabebackend.");
}
HostRuntimeSession::~HostRuntimeSession() {
    shutdown();
}

void HostRuntimeSession::inject(const HostRuntimeEvent& event) {
    if (state_ == HostRuntimeState::Shutdown)
        throw std::logic_error("Hostruntime ist bereits heruntergefahren.");
    try {
        if (event.sequence == 0u || event.sequence <= last_sequence_ ||
            event.guest_cycle < last_guest_cycle_ ||
            event.guest_cycle < scheduler_.current_cycle()) {
            throw std::invalid_argument(
                "Hostereignis ist nicht streng sequenziert oder gastzeitmonoton.");
        }
        last_sequence_ = event.sequence;
        last_guest_cycle_ = event.guest_cycle;
        audio_.bind_command_frame(media_clock_.audio_tick_count());
        switch (event.kind) {
        case HostRuntimeEventKind::Resume:
        case HostRuntimeEventKind::FocusGained:
            audio_.resume();
            if (pacer_ != nullptr) pacer_->resume(scheduler_.current_cycle());
            media_clock_.start();
            state_ = HostRuntimeState::Running;
            break;
        case HostRuntimeEventKind::Pause:
        case HostRuntimeEventKind::FocusLost:
            if (event.kind == HostRuntimeEventKind::FocusLost)
                input_->inject(event.sequence, event.guest_cycle, {});
            media_clock_.stop();
            if (pacer_ != nullptr) pacer_->pause(scheduler_.current_cycle());
            audio_.pause();
            state_ = HostRuntimeState::Paused;
            break;
        case HostRuntimeEventKind::Controller:
            input_->inject(event.sequence, event.guest_cycle, event.controller);
            break;
        case HostRuntimeEventKind::Shutdown:
            shutdown();
            require_clean_shutdown();
            break;
        }
        ++processed_events_;
    } catch (...) {
        shutdown();
        throw;
    }
}

void HostRuntimeSession::shutdown() noexcept {
    if (state_ == HostRuntimeState::Shutdown) return;
    media_clock_.stop();
    scheduler_.clear();
    audio_.bind_command_frame(media_clock_.audio_tick_count());
    audio_.shutdown();
    if (pacer_ != nullptr) pacer_->shutdown();
    state_ = HostRuntimeState::Shutdown;
    if (shutdown_callback_) {
        try {
            shutdown_callback_();
        } catch (...) {
            shutdown_error_ = "persistent-storage-save-failed";
        }
    }
}
void HostRuntimeSession::require_clean_shutdown() const {
    if (shutdown_error_) throw std::runtime_error(*shutdown_error_);
}
HostRuntimeState HostRuntimeSession::state() const noexcept {
    return state_;
}
std::uint64_t HostRuntimeSession::processed_events() const noexcept {
    return processed_events_;
}
const std::optional<std::string>& HostRuntimeSession::shutdown_error() const noexcept {
    return shutdown_error_;
}

} // namespace katana::runtime
