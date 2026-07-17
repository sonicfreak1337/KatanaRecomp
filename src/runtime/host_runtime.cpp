#include "katana/runtime/host_runtime.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
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
                                       HostAudioOutput& audio)
    : scheduler_(scheduler), media_clock_(media_clock), input_(std::move(input)), audio_(audio) {
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
            media_clock_.start();
            state_ = HostRuntimeState::Running;
            break;
        case HostRuntimeEventKind::Pause:
        case HostRuntimeEventKind::FocusLost:
            media_clock_.stop();
            audio_.pause();
            state_ = HostRuntimeState::Paused;
            break;
        case HostRuntimeEventKind::Controller:
            input_->inject(event.sequence, event.guest_cycle, event.controller);
            break;
        case HostRuntimeEventKind::Shutdown:
            shutdown();
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
    state_ = HostRuntimeState::Shutdown;
}
HostRuntimeState HostRuntimeSession::state() const noexcept {
    return state_;
}
std::uint64_t HostRuntimeSession::processed_events() const noexcept {
    return processed_events_;
}

} // namespace katana::runtime
