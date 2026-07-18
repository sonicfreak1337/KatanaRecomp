#include "katana/runtime/host_runtime.hpp"

#include "katana/io/json_report.hpp"

#include <chrono>
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

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

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
