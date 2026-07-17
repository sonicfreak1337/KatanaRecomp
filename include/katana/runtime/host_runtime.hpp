#pragma once

#include "katana/runtime/aica.hpp"
#include "katana/runtime/maple.hpp"
#include "katana/runtime/media_clock.hpp"

#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t native_host_runtime_contract_version = 1u;

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

class HostRuntimeSession final {
  public:
    HostRuntimeSession(EventScheduler& scheduler,
                       DreamcastMediaClock& media_clock,
                       std::shared_ptr<InjectedHostInput> input,
                       HostAudioOutput& audio);
    ~HostRuntimeSession();
    HostRuntimeSession(const HostRuntimeSession&) = delete;
    HostRuntimeSession& operator=(const HostRuntimeSession&) = delete;
    void inject(const HostRuntimeEvent& event);
    void shutdown() noexcept;
    [[nodiscard]] HostRuntimeState state() const noexcept;
    [[nodiscard]] std::uint64_t processed_events() const noexcept;

  private:
    EventScheduler& scheduler_;
    DreamcastMediaClock& media_clock_;
    std::shared_ptr<InjectedHostInput> input_;
    HostAudioOutput& audio_;
    HostRuntimeState state_ = HostRuntimeState::Paused;
    std::uint64_t last_sequence_ = 0u;
    std::uint64_t last_guest_cycle_ = 0u;
    std::uint64_t processed_events_ = 0u;
};

} // namespace katana::runtime
