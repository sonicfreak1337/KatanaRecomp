#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace katana::runtime {

class AicaExecutionController;

inline constexpr std::uint32_t aica_register_physical_base = 0x00700000u;
inline constexpr std::size_t aica_register_size = 0x00008000u;
inline constexpr std::size_t aica_channel_count = 64u;
inline constexpr std::size_t aica_channel_register_stride = 0x80u;
inline constexpr std::uint32_t aica_common_register_base = 0x2800u;

class AicaRegisterFile final {
  public:
    explicit AicaRegisterFile(std::shared_ptr<AicaExecutionController> execution = {},
                              std::shared_ptr<LinearMemoryDevice> ram = {});
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width) const;
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t write_count() const noexcept;
    [[nodiscard]] std::vector<std::int16_t> render_audio(std::size_t frame_count,
                                                         std::uint32_t sample_rate);
    [[nodiscard]] std::size_t active_channel_count() const noexcept;
    [[nodiscard]] std::uint64_t rendered_buffer_count() const noexcept;
    [[nodiscard]] std::uint64_t rendered_frame_count() const noexcept;

  private:
    struct ChannelRuntime {
        std::uint64_t phase = 0u;
        std::uint32_t adpcm_position = 0u;
        std::int32_t adpcm_predictor = 0;
        std::int32_t adpcm_step = 127;
        bool active = false;
    };
    [[nodiscard]] static std::size_t width_bytes(MemoryAccessWidth width) noexcept;
    void check(std::uint32_t offset, MemoryAccessWidth width) const;
    std::array<std::uint8_t, aica_register_size> registers_{};
    std::uint64_t writes_ = 0u;
    std::shared_ptr<AicaExecutionController> execution_;
    std::shared_ptr<LinearMemoryDevice> ram_;
    std::array<ChannelRuntime, aica_channel_count> channels_{};
    std::uint64_t rendered_buffers_ = 0u;
    std::uint64_t rendered_frames_ = 0u;
};

enum class AicaSampleFormat : std::uint8_t { Pcm16, Pcm8, Adpcm4 };

class AicaSampleDecoder final {
  public:
    explicit AicaSampleDecoder(AicaSampleFormat format) noexcept;
    [[nodiscard]] std::vector<std::int16_t> decode(std::span<const std::uint8_t> source,
                                                   std::size_t sample_count);
    void reset() noexcept;
    [[nodiscard]] std::int32_t predictor() const noexcept;
    [[nodiscard]] std::int32_t step() const noexcept;

  private:
    [[nodiscard]] std::int16_t decode_adpcm_nibble(std::uint8_t nibble) noexcept;
    AicaSampleFormat format_ = AicaSampleFormat::Pcm16;
    std::int32_t predictor_ = 0;
    std::int32_t step_ = 127;
};

inline constexpr std::uint32_t aica_unity_gain = 32768u;
inline constexpr std::int32_t aica_pan_left = -32768;
inline constexpr std::int32_t aica_pan_center = 0;
inline constexpr std::int32_t aica_pan_right = 32768;

struct AicaVoice {
    std::span<const std::int16_t> samples;
    std::uint32_t gain = aica_unity_gain;
    std::int32_t pan = aica_pan_center;
};

class AicaMixer final {
  public:
    [[nodiscard]] std::vector<std::int16_t> mix(std::span<const AicaVoice> voices,
                                                std::size_t frame_count) const;
};

class AicaAudioBackend {
  public:
    virtual ~AicaAudioBackend() = default;
    virtual void submit(std::span<const std::int16_t> interleaved_stereo,
                        std::uint32_t sample_rate) = 0;
};

class RecordingAicaAudioBackend final : public AicaAudioBackend {
  public:
    void submit(std::span<const std::int16_t> interleaved_stereo,
                std::uint32_t sample_rate) override;
    [[nodiscard]] std::uint64_t submitted_buffers() const noexcept;
    [[nodiscard]] std::uint64_t submitted_frames() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] const std::vector<std::int16_t>& last_buffer() const noexcept;

  private:
    std::uint64_t submitted_buffers_ = 0u;
    std::uint64_t submitted_frames_ = 0u;
    std::uint32_t sample_rate_ = 0u;
    std::vector<std::int16_t> last_buffer_;
};

enum class AicaArm7Mode : std::uint8_t { HighLevelAudio, LowLevelArm7 };

class AicaTimer final {
  public:
    void configure(std::uint8_t initial_counter, std::uint8_t divider_scale, bool enabled);
    [[nodiscard]] std::uint64_t tick(std::uint64_t audio_cycles) noexcept;
    [[nodiscard]] std::uint8_t counter() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;

  private:
    std::uint64_t remainder_ = 0u;
    std::uint32_t divisor_ = 1u;
    std::uint8_t counter_ = 0u;
    bool enabled_ = false;
};

class AicaInterruptState final {
  public:
    void set_observer(std::function<void()> observer);
    void set_enabled(std::uint32_t mask);
    void request(std::uint32_t mask);
    void acknowledge(std::uint32_t mask) noexcept;
    [[nodiscard]] std::uint32_t pending() const noexcept;
    [[nodiscard]] std::uint32_t enabled() const noexcept;
    [[nodiscard]] bool asserted() const noexcept;

  private:
    std::uint32_t enabled_ = 0u;
    std::uint32_t pending_ = 0u;
    std::function<void()> observer_;
};

class AicaExecutionController final {
  public:
    static constexpr std::size_t timer_count = 3u;
    static constexpr std::uint32_t timer_interrupt_base = 1u << 6u;
    explicit AicaExecutionController(EventScheduler* scheduler = nullptr,
                                     std::uint64_t guest_clock_hz = 200'000'000u,
                                     std::uint64_t audio_clock_hz = 44'100u);
    ~AicaExecutionController();
    AicaExecutionController(const AicaExecutionController&) = delete;
    AicaExecutionController& operator=(const AicaExecutionController&) = delete;
    void set_mode(AicaArm7Mode mode);
    [[nodiscard]] AicaArm7Mode mode() const noexcept;
    [[nodiscard]] bool arm7_executes_instructions() const noexcept;
    void set_arm7_reset_asserted(bool asserted) noexcept;
    [[nodiscard]] bool arm7_reset_asserted() const noexcept;
    [[nodiscard]] AicaTimer& timer(std::size_t index);
    [[nodiscard]] const AicaTimer& timer(std::size_t index) const;
    [[nodiscard]] AicaInterruptState& interrupts() noexcept;
    [[nodiscard]] const AicaInterruptState& interrupts() const noexcept;
    void set_dma_request_observer(std::function<void()> observer);
    void tick(std::uint64_t audio_cycles);

  private:
    void schedule_tick();
    void handle_tick(SchedulerEventId event_id);
    void handle_scheduler_reset() noexcept;
    AicaArm7Mode mode_ = AicaArm7Mode::HighLevelAudio;
    bool arm7_reset_asserted_ = false;
    std::array<AicaTimer, timer_count> timers_{};
    AicaInterruptState interrupts_;
    EventScheduler* scheduler_ = nullptr;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::optional<SchedulerEventId> tick_event_;
    std::function<void()> dma_request_observer_;
    std::uint64_t guest_cycles_per_tick_ = 0u;
    static constexpr std::uint64_t audio_cycles_per_tick = 256u;
};

[[nodiscard]] std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory);
[[nodiscard]] std::shared_ptr<AicaRegisterFile>
map_aica_registers(Memory& memory, std::shared_ptr<AicaExecutionController> execution);
[[nodiscard]] std::shared_ptr<AicaRegisterFile>
map_aica_registers(Memory& memory,
                   std::shared_ptr<AicaExecutionController> execution,
                   std::shared_ptr<LinearMemoryDevice> ram);

} // namespace katana::runtime
