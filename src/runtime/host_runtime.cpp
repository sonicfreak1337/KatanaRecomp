#include "katana/runtime/host_runtime.hpp"

#include "katana/io/json_report.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
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
    bool paused_ = false;
    bool shutdown_ = false;
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
                                         HostWaitFor wait_for)
    : config_(config), wall_now_(std::move(wall_now)),
      thread_cpu_now_(std::move(thread_cpu_now)), wait_for_(std::move(wait_for)) {
    if (config_.target_cpu_percent == 0u || config_.target_cpu_percent > 100u)
        throw std::invalid_argument("Host-Lastbegrenzung besitzt eine ungueltige Zielquote.");
    if (!enabled()) return;
    if (config_.accounting_window_ns == 0u)
        throw std::invalid_argument(
            "Host-Lastbegrenzung besitzt kein Abrechnungsfenster.");
    if (config_.maximum_wait_ns == 0u ||
        config_.maximum_wait_ns > host_workload_limiter_maximum_wait_ceiling_ns)
        throw std::invalid_argument(
            "Host-Lastbegrenzung besitzt ein ungueltiges Warteintervall.");
    if (!wall_now_) wall_now_ = monotonic_now_ns;
    if (!thread_cpu_now_) thread_cpu_now_ = thread_cpu_now_ns;
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
    auto thread_cpu_now = thread_cpu_now_();
    if (!initialized_) {
        rebase(wall_now, thread_cpu_now);
        return;
    }
    if (wall_now < last_wall_ns_ || thread_cpu_now < last_thread_cpu_ns_) {
        rebase(wall_now, thread_cpu_now);
        return;
    }
    observe_sample(wall_now, thread_cpu_now);

    for (;;) {
        const auto wall_elapsed = wall_now - anchor_wall_ns_;
        const auto thread_cpu_elapsed =
            thread_cpu_now - anchor_thread_cpu_ns_;
        const auto required_wall =
            required_wall_time_ns(
                thread_cpu_elapsed, config_.target_cpu_percent);
        if (required_wall == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error(
                "Host-Lastbegrenzungsschuld ist nicht darstellbar.");
        if (required_wall <= wall_elapsed) {
            if (wall_elapsed >= config_.accounting_window_ns)
                rebase(wall_now, thread_cpu_now);
            return;
        }
        const auto wait_duration =
            std::min(required_wall - wall_elapsed, config_.maximum_wait_ns);
        wait_for_(wait_duration);
        increment(wait_calls_);

        const auto wall_after_wait = wall_now_();
        const auto thread_cpu_after_wait = thread_cpu_now_();
        if (wall_after_wait >= wall_now)
            saturating_add(wait_time_ns_, wall_after_wait - wall_now);
        if (wall_after_wait < wall_now || thread_cpu_after_wait < thread_cpu_now) {
            rebase(wall_after_wait, thread_cpu_after_wait);
            return;
        }
        if (wall_after_wait == wall_now)
            throw std::runtime_error(
                "Host-Lastbegrenzungswait erzeugte keinen Zeitfortschritt.");
        wall_now = wall_after_wait;
        thread_cpu_now = thread_cpu_after_wait;
        observe_sample(wall_now, thread_cpu_now);
    }
}

void HostWorkloadLimiter::reset() noexcept {
    anchor_wall_ns_ = 0u;
    anchor_thread_cpu_ns_ = 0u;
    last_wall_ns_ = 0u;
    last_thread_cpu_ns_ = 0u;
    initialized_ = false;
}

bool HostWorkloadLimiter::enabled() const noexcept {
    return config_.target_cpu_percent < 100u;
}

bool HostWorkloadLimiter::initialized() const noexcept {
    return initialized_;
}

std::uint32_t HostWorkloadLimiter::target_cpu_percent() const noexcept {
    return config_.target_cpu_percent;
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
HostWorkloadLimiter::measured_cpu_percent_milli() const noexcept {
    if (measured_wall_time_ns_ == 0u) return 0u;
    constexpr std::uint64_t scale = 100'000u;
    if (measured_thread_cpu_time_ns_ >
        std::numeric_limits<std::uint64_t>::max() / scale)
        return std::numeric_limits<std::uint64_t>::max();
    return measured_thread_cpu_time_ns_ * scale /
           measured_wall_time_ns_;
}

std::string HostWorkloadLimiter::serialize_status_json() const {
    std::ostringstream output;
    katana::io::write_json_report_header(
        output, "katana-host-workload-limiter-v1", "host-workload-limiter");
    output << ",\"contract_version\":" << host_workload_limiter_contract_version
           << ",\"enabled\":" << (enabled() ? "true" : "false")
           << ",\"initialized\":" << (initialized_ ? "true" : "false")
           << ",\"target_cpu_percent\":" << config_.target_cpu_percent
           << ",\"accounting_window_ns\":" << config_.accounting_window_ns
           << ",\"maximum_wait_ns\":" << config_.maximum_wait_ns
           << ",\"wait_calls\":" << wait_calls_
           << ",\"wait_time_ns\":" << wait_time_ns_
           << ",\"measured_wall_time_ns\":" << measured_wall_time_ns_
           << ",\"measured_thread_cpu_time_ns\":"
           << measured_thread_cpu_time_ns_
           << ",\"measured_cpu_percent_milli\":"
           << measured_cpu_percent_milli() << '}';
    return output.str();
}

void HostWorkloadLimiter::observe_sample(
    const std::uint64_t wall_ns,
    const std::uint64_t thread_cpu_ns) noexcept {
    saturating_add(measured_wall_time_ns_, wall_ns - last_wall_ns_);
    saturating_add(
        measured_thread_cpu_time_ns_, thread_cpu_ns - last_thread_cpu_ns_);
    last_wall_ns_ = wall_ns;
    last_thread_cpu_ns_ = thread_cpu_ns;
}

void HostWorkloadLimiter::rebase(const std::uint64_t wall_ns,
                                 const std::uint64_t thread_cpu_ns) noexcept {
    anchor_wall_ns_ = wall_ns;
    anchor_thread_cpu_ns_ = thread_cpu_ns;
    last_wall_ns_ = wall_ns;
    last_thread_cpu_ns_ = thread_cpu_ns;
    initialized_ = true;
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

bool native_audio_available() noexcept {
#ifdef _WIN32
    return waveOutGetNumDevs() != 0u;
#else
    return false;
#endif
}

std::unique_ptr<HostAudioOutput> create_native_audio_output() {
#ifdef _WIN32
    return std::make_unique<Win32AudioOutput>();
#else
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
