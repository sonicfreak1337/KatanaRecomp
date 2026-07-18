#pragma once

#include "katana/runtime/aica.hpp"
#include "katana/runtime/maple.hpp"
#include "katana/runtime/media_clock.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace katana::runtime {

inline constexpr std::uint32_t native_host_runtime_contract_version = 2u;
inline constexpr std::uint32_t host_pacing_contract_version = 1u;

enum class HostPacingError : std::uint8_t {
    None,
    GuestCycleRegression,
    HostClockRegression,
    DeadlineOverflow,
    WaitReturnedEarly
};

struct HostPacingFirstError {
    HostPacingError error = HostPacingError::None;
    std::uint64_t guest_cycle = 0u;
};

struct HostPacingConfig {
    std::uint64_t guest_cycles_per_second = dreamcast_guest_cycles_per_second;
};

using HostMonotonicNow = std::function<std::uint64_t()>;
using HostWaitUntil = std::function<void(std::uint64_t)>;

class HostPacingException final : public std::runtime_error {
  public:
    HostPacingException(HostPacingError error, std::uint64_t guest_cycle);
    [[nodiscard]] HostPacingError error() const noexcept;
    [[nodiscard]] std::uint64_t guest_cycle() const noexcept;
    [[nodiscard]] std::string serialize_json() const;

  private:
    HostPacingError error_ = HostPacingError::None;
    std::uint64_t guest_cycle_ = 0u;
};

class HostPacer final {
  public:
    explicit HostPacer(HostPacingConfig config = {},
                       HostMonotonicNow now = {},
                       HostWaitUntil wait_until = {});
    void resume(std::uint64_t guest_cycle);
    void pause(std::uint64_t guest_cycle);
    void pace(std::uint64_t guest_cycle);
    void shutdown() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool shutdown_complete() const noexcept;
    [[nodiscard]] std::uint64_t wait_calls() const noexcept;
    [[nodiscard]] std::uint64_t late_deadlines() const noexcept;
    [[nodiscard]] std::uint64_t last_guest_cycle() const noexcept;
    [[nodiscard]] const std::optional<HostPacingFirstError>& first_error() const noexcept;
    [[nodiscard]] std::string serialize_status_json() const;

  private:
    [[noreturn]] void fail(HostPacingError error, std::uint64_t guest_cycle);
    HostPacingConfig config_;
    HostMonotonicNow now_;
    HostWaitUntil wait_until_;
    std::uint64_t anchor_guest_cycle_ = 0u;
    std::uint64_t anchor_host_ns_ = 0u;
    std::uint64_t last_guest_cycle_ = 0u;
    std::uint64_t last_host_ns_ = 0u;
    std::uint64_t wait_calls_ = 0u;
    std::uint64_t late_deadlines_ = 0u;
    std::optional<HostPacingFirstError> first_error_;
    bool initialized_ = false;
    bool running_ = false;
    bool shutdown_ = false;
};

[[nodiscard]] const char* host_pacing_error_name(HostPacingError value) noexcept;

class HostAudioOutput : public AicaAudioBackend {
  public:
    ~HostAudioOutput() override = default;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void shutdown() noexcept = 0;
    [[nodiscard]] virtual bool paused() const noexcept = 0;
    [[nodiscard]] virtual bool shutdown_complete() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t submitted_buffers() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t submitted_frames() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t deterministic_hash() const noexcept = 0;
};

class RecordingHostAudioOutput final : public HostAudioOutput {
  public:
    void submit(std::span<const std::int16_t> samples, std::uint32_t sample_rate) override;
    void pause() override;
    void resume() override;
    void shutdown() noexcept override;
    [[nodiscard]] bool paused() const noexcept override;
    [[nodiscard]] bool shutdown_complete() const noexcept override;
    [[nodiscard]] std::uint64_t submitted_buffers() const noexcept override;
    [[nodiscard]] std::uint64_t submitted_frames() const noexcept override;
    [[nodiscard]] std::uint64_t deterministic_hash() const noexcept override;

  private:
    std::uint64_t hash_ = 1469598103934665603ull;
    std::uint64_t buffers_ = 0u;
    std::uint64_t frames_ = 0u;
    bool paused_ = false;
    bool shutdown_ = false;
};

[[nodiscard]] bool native_audio_available() noexcept;
[[nodiscard]] std::unique_ptr<HostAudioOutput> create_native_audio_output();

class InjectedHostInput final : public HostInputBackend {
  public:
    void inject(std::uint64_t sequence, std::uint64_t guest_cycle, ControllerState state);
    [[nodiscard]] ControllerState sample(std::uint64_t frame) override;
    [[nodiscard]] std::uint64_t injected_events() const noexcept;
    [[nodiscard]] std::uint64_t sampled_frames() const noexcept;

  private:
    ControllerState state_{};
    std::uint64_t last_sequence_ = 0u;
    std::uint64_t last_guest_cycle_ = 0u;
    std::uint64_t injected_events_ = 0u;
    std::uint64_t sampled_frames_ = 0u;
};

enum class HostRuntimeEventKind : std::uint8_t {
    Resume,
    Pause,
    FocusGained,
    FocusLost,
    Controller,
    Shutdown
};

struct HostRuntimeEvent {
    std::uint64_t sequence = 0u;
    std::uint64_t guest_cycle = 0u;
    HostRuntimeEventKind kind = HostRuntimeEventKind::Pause;
    ControllerState controller{};
};

enum class HostRuntimeState : std::uint8_t { Paused, Running, Shutdown };

using HostShutdownCallback = std::function<void()>;

class HostRuntimeSession final {
  public:
    HostRuntimeSession(EventScheduler& scheduler,
                       DreamcastMediaClock& media_clock,
                       std::shared_ptr<InjectedHostInput> input,
                       HostAudioOutput& audio,
                       HostPacer* pacer = nullptr,
                       HostShutdownCallback shutdown_callback = {});
    ~HostRuntimeSession();
    HostRuntimeSession(const HostRuntimeSession&) = delete;
    HostRuntimeSession& operator=(const HostRuntimeSession&) = delete;
    void inject(const HostRuntimeEvent& event);
    void shutdown() noexcept;
    void require_clean_shutdown() const;
    [[nodiscard]] HostRuntimeState state() const noexcept;
    [[nodiscard]] std::uint64_t processed_events() const noexcept;
    [[nodiscard]] const std::optional<std::string>& shutdown_error() const noexcept;

  private:
    EventScheduler& scheduler_;
    DreamcastMediaClock& media_clock_;
    std::shared_ptr<InjectedHostInput> input_;
    HostAudioOutput& audio_;
    HostPacer* pacer_ = nullptr;
    HostShutdownCallback shutdown_callback_;
    HostRuntimeState state_ = HostRuntimeState::Paused;
    std::uint64_t last_sequence_ = 0u;
    std::uint64_t last_guest_cycle_ = 0u;
    std::uint64_t processed_events_ = 0u;
    std::optional<std::string> shutdown_error_;
};

} // namespace katana::runtime
