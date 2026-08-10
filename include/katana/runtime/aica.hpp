#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/observation_restore_policy.hpp"
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
class AicaArm7Core;

inline constexpr std::uint32_t aica_register_physical_base = 0x00700000u;
inline constexpr std::size_t aica_register_size = 0x00008000u;
inline constexpr std::uint32_t aica_rtc_physical_base = 0x00710000u;
inline constexpr std::size_t aica_rtc_register_size = 0x0000000Cu;
inline constexpr std::uint32_t aica_rtc_high_offset = 0x00u;
inline constexpr std::uint32_t aica_rtc_low_offset = 0x04u;
inline constexpr std::uint32_t aica_rtc_control_offset = 0x08u;
// 2000-01-01 in seconds since the Dreamcast RTC epoch (1950-01-01).
inline constexpr std::uint32_t aica_rtc_default_seconds = 1'577'836'800u;
inline constexpr std::size_t aica_channel_count = 64u;
inline constexpr std::size_t aica_channel_register_stride = 0x80u;
inline constexpr std::uint32_t aica_common_register_base = 0x2800u;
inline constexpr std::uint32_t dreamcast_aica_tick_event_channel = 1u;
inline constexpr std::uint64_t dreamcast_aica_tick_event_token_v1 = 1u;

enum class AicaSampleFormat : std::uint8_t { Pcm16, Pcm8, Adpcm4 };

enum class AicaVoiceError : std::uint8_t {
    Pcm16OutOfRange,
    Pcm8OutOfRange,
    AdpcmOutOfRange,
};

struct AicaVoiceFirstError {
    AicaVoiceError error = AicaVoiceError::Pcm16OutOfRange;
    AicaSampleFormat format = AicaSampleFormat::Pcm16;
    std::uint32_t channel = 0u;
    std::uint64_t sample_address = 0u;
    std::uint64_t rendered_frame = 0u;

    [[nodiscard]] bool operator==(const AicaVoiceFirstError&) const = default;
};

struct AicaChannelRuntimeSnapshot {
    std::uint64_t phase = 0u;
    std::uint32_t adpcm_position = 0u;
    std::int32_t adpcm_predictor = 0;
    std::int32_t adpcm_step = 127;
    bool active = false;
    bool looped = false;

    [[nodiscard]] bool operator==(const AicaChannelRuntimeSnapshot&) const = default;
};

struct AicaRegisterSnapshot {
    std::array<std::uint8_t, aica_register_size> registers{};
    std::array<AicaChannelRuntimeSnapshot, aica_channel_count> channels{};
    std::uint64_t writes = 0u;
    std::uint64_t rendered_buffers = 0u;
    std::uint64_t rendered_frames = 0u;
    std::uint64_t voice_errors = 0u;
    std::optional<AicaVoiceFirstError> first_voice_error;

    [[nodiscard]] bool operator==(const AicaRegisterSnapshot&) const = default;
};

struct AicaRtcSnapshot {
    std::uint64_t scheduler_cycle = 0u;
    std::uint64_t guest_clock_hz = 0u;
    std::uint64_t base_cycle = 0u;
    std::uint32_t initial_seconds = 0u;
    std::uint32_t base_seconds = 0u;
    std::uint32_t counter = 0u;
    std::uint32_t write_latch = 0u;
    bool write_enabled = false;

    [[nodiscard]] bool operator==(const AicaRtcSnapshot&) const = default;
};

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
    [[nodiscard]] std::uint64_t voice_error_count() const noexcept;
    [[nodiscard]] std::optional<AicaVoiceFirstError> first_voice_error() const noexcept;
    // Run-local host telemetry. These counters are intentionally not part of a
    // portable guest-state snapshot.
    [[nodiscard]] std::size_t render_job_capacity() const noexcept;
    [[nodiscard]] std::size_t last_render_job_count() const noexcept;
    [[nodiscard]] std::uint64_t parallel_rendered_buffer_count() const noexcept;
    [[nodiscard]] AicaRegisterSnapshot snapshot() const noexcept;
    void validate_state_restore(const AicaRegisterSnapshot& state) const;
    void restore_state_passive(AicaRegisterSnapshot state);
    void commit_validated_state_restore(
        AicaRegisterSnapshot state) noexcept;

  private:
    friend class AicaArm7Core;
    struct ChannelRuntime {
        std::uint64_t phase = 0u;
        std::uint32_t adpcm_position = 0u;
        std::int32_t adpcm_predictor = 0;
        std::int32_t adpcm_step = 127;
        bool active = false;
        mutable bool looped = false;
    };
    [[nodiscard]] static std::size_t width_bytes(MemoryAccessWidth width) noexcept;
    void check(std::uint32_t offset, MemoryAccessWidth width) const;
    [[nodiscard]] std::uint32_t read_arm(std::uint32_t offset,
                                         MemoryAccessWidth width) const;
    void write_arm(std::uint32_t offset,
                   std::uint32_t value,
                   MemoryAccessWidth width);
    void record_voice_error(AicaVoiceError error,
                            AicaSampleFormat format,
                            std::size_t channel,
                            std::uint64_t sample_address,
                            std::uint64_t rendered_frame) noexcept;
    std::array<std::uint8_t, aica_register_size> registers_{};
    std::uint64_t writes_ = 0u;
    std::shared_ptr<AicaExecutionController> execution_;
    std::shared_ptr<LinearMemoryDevice> ram_;
    std::array<ChannelRuntime, aica_channel_count> channels_{};
    std::uint64_t rendered_buffers_ = 0u;
    std::uint64_t rendered_frames_ = 0u;
    std::uint64_t voice_errors_ = 0u;
    std::optional<AicaVoiceFirstError> first_voice_error_;
    std::size_t last_render_jobs_ = 1u;
    std::uint64_t parallel_rendered_buffers_ = 0u;
};

class AicaRtc final {
  public:
    explicit AicaRtc(EventScheduler* scheduler = nullptr,
                     std::uint64_t guest_clock_hz = dreamcast_guest_cycles_per_second,
                     std::uint32_t initial_seconds = aica_rtc_default_seconds);
    ~AicaRtc();
    AicaRtc(const AicaRtc&) = delete;
    AicaRtc& operator=(const AicaRtc&) = delete;

    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width) const;
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);
    void reset() noexcept;
    [[nodiscard]] std::uint32_t counter() const noexcept;
    [[nodiscard]] bool write_enabled() const noexcept;
    [[nodiscard]] AicaRtcSnapshot snapshot() const noexcept;
    void validate_state_restore(const AicaRtcSnapshot& state) const;
    void validate_state_restore(
        const AicaRtcSnapshot& state,
        std::uint64_t expected_scheduler_cycle) const;
    void restore_state_passive(AicaRtcSnapshot state);
    void commit_validated_state_restore(AicaRtcSnapshot state) noexcept;

  private:
    static void check(std::uint32_t offset, MemoryAccessWidth width);
    void commit_elapsed() noexcept;
    void handle_scheduler_reset() noexcept;
    EventScheduler* scheduler_ = nullptr;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::uint64_t guest_clock_hz_ = dreamcast_guest_cycles_per_second;
    std::uint64_t base_cycle_ = 0u;
    std::uint32_t initial_seconds_ = aica_rtc_default_seconds;
    std::uint32_t base_seconds_ = aica_rtc_default_seconds;
    std::uint32_t write_latch_ = aica_rtc_default_seconds;
    bool write_enabled_ = false;
};

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
enum class AicaExecutionError : std::uint8_t {
    None,
    TickScheduleFailure,
    Arm7ExecutionFailure,
};

struct AicaArm7BlockTransferSnapshot {
    std::uint32_t address = 0u;
    std::uint32_t r15_offset = 0u;
    std::uint32_t last_bank = 0u;
    std::uint32_t base_address = 0u;
    std::uint32_t cycle = 0u;
    std::uint32_t register_count = 0u;

    [[nodiscard]] bool operator==(const AicaArm7BlockTransferSnapshot&) const = default;
};

// Portable guest-visible ARM7TDMI continuation. Debug rings, callback pointers
// and host-only lookup tables deliberately stay outside the handoff contract.
struct AicaArm7Snapshot {
    std::array<std::uint32_t, 37u> registers{};
    std::array<std::uint32_t, 5u> prefetch_opcodes{};
    std::uint32_t prefetch_pc = 0xFFFFFFFFu;
    std::uint32_t instruction_cycles = 0u;
    std::uint32_t phased_opcode = 0u;
    std::uint32_t phased_operation = 1u;
    std::uint32_t phase = 0u;
    AicaArm7BlockTransferSnapshot block;
    std::uint64_t executed_instructions = 0u;
    std::uint64_t executed_cycles = 0u;
    std::uint64_t cycle_debt = 0u;
    bool next_fetch_sequential = false;
    bool waiting_for_interrupt = false;
    bool enabled = false;
    bool faulted = false;

    [[nodiscard]] bool operator==(const AicaArm7Snapshot&) const = default;
};

class AicaTimer final {
  public:
    void configure(std::uint8_t initial_counter, std::uint8_t divider_scale, bool enabled);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t tick(std::uint64_t audio_cycles) noexcept;
    [[nodiscard]] std::uint8_t counter() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    struct Snapshot {
        std::uint64_t remainder = 0u;
        std::uint32_t divisor = 1u;
        std::uint8_t counter = 0u;
        bool enabled = false;

        [[nodiscard]] bool operator==(const Snapshot&) const = default;
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;
    void validate_state_restore(const Snapshot& state) const;
    void restore_state_passive(Snapshot state);
    void commit_validated_state_restore(Snapshot state) noexcept;

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
    void reset() noexcept;
    [[nodiscard]] std::uint32_t pending() const noexcept;
    [[nodiscard]] std::uint32_t enabled() const noexcept;
    [[nodiscard]] bool asserted() const noexcept;
    struct Snapshot {
        std::uint32_t enabled = 0u;
        std::uint32_t pending = 0u;
        bool asserted = false;

        [[nodiscard]] bool operator==(const Snapshot&) const = default;
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;
    void validate_state_restore(const Snapshot& state) const;
    // Direct assignment deliberately avoids the IRQ publication observer.
    void restore_state_passive(Snapshot state);
    void commit_validated_state_restore(Snapshot state) noexcept;

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
    void reset() noexcept;
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
    void request_dma();
    void tick(std::uint64_t audio_cycles);
    struct Snapshot {
        AicaArm7Mode mode = AicaArm7Mode::HighLevelAudio;
        bool arm7_reset_asserted = false;
        std::array<AicaTimer::Snapshot, timer_count> timers{};
        AicaInterruptState::Snapshot interrupts{};
        AicaInterruptState::Snapshot sound_interrupts{};
        std::array<std::uint8_t, 3u> sound_interrupt_levels{};
        std::uint8_t arm_interrupt_level = 0u;
        bool arm_interrupt_output = false;
        AicaArm7Snapshot arm7;
        std::optional<SchedulerEventId> tick_event;
        // SchedulerEventId is process-local. Portable state carries the typed
        // AICA tick event separately and sets this flag until rehydration.
        bool tick_event_rehydration_pending = false;
        AicaExecutionError error = AicaExecutionError::None;
        std::uint64_t guest_cycles_per_tick = 0u;

        [[nodiscard]] bool operator==(const Snapshot&) const = default;
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;
    void validate_state_restore(const Snapshot& state) const;
    void restore_state_passive(Snapshot state);
    void commit_validated_state_restore(Snapshot state) noexcept;
    [[nodiscard]] SchedulerEventId rehydrate_scheduled_event(
        std::uint64_t guest_cycle,
        std::uint32_t channel,
        std::uint64_t token);
    [[nodiscard]] SchedulerCallback
    make_rehydrated_scheduled_event_callback(
        std::uint32_t channel,
        std::uint64_t token);
    void commit_rehydrated_scheduled_event(
        SchedulerEventId event_id,
        std::uint32_t channel,
        std::uint64_t token) noexcept;
    [[nodiscard]] bool event_rehydration_pending() const noexcept;

  private:
    friend class AicaRegisterFile;
    friend class AicaArm7Core;
    friend std::shared_ptr<AicaRegisterFile>
    map_aica_registers(Memory&,
                       std::shared_ptr<AicaExecutionController>,
                       std::shared_ptr<LinearMemoryDevice>);
    void schedule_tick();
    void handle_tick(SchedulerEventId event_id);
    void handle_scheduler_reset() noexcept;
    void bind_arm7_bus(const std::shared_ptr<AicaRegisterFile>& registers,
                       const std::shared_ptr<LinearMemoryDevice>& ram);
    void set_sound_interrupt_enabled(std::uint32_t mask);
    void request_sound_interrupt(std::uint32_t mask);
    void acknowledge_sound_interrupt(std::uint32_t mask) noexcept;
    void set_sound_interrupt_levels(std::array<std::uint8_t, 3u> levels) noexcept;
    [[nodiscard]] std::uint32_t sound_interrupt_pending() const noexcept;
    [[nodiscard]] std::uint8_t arm_interrupt_level() const noexcept;
    void accept_arm_interrupt() noexcept;
    void refresh_arm_interrupt() noexcept;
    AicaArm7Mode mode_ = AicaArm7Mode::HighLevelAudio;
    bool arm7_reset_asserted_ = false;
    std::array<AicaTimer, timer_count> timers_{};
    AicaInterruptState interrupts_;
    AicaInterruptState sound_interrupts_;
    std::array<std::uint8_t, 3u> sound_interrupt_levels_{};
    std::uint8_t arm_interrupt_level_ = 0u;
    bool arm_interrupt_output_ = false;
    std::unique_ptr<AicaArm7Core> arm7_;
    EventScheduler* scheduler_ = nullptr;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::optional<SchedulerEventId> tick_event_;
    bool tick_event_rehydration_pending_ = false;
    AicaExecutionError error_ = AicaExecutionError::None;
    std::function<void()> dma_request_observer_;
    std::uint64_t guest_cycles_per_tick_ = 0u;
    static constexpr std::uint64_t audio_cycles_per_tick = 256u;
};

inline constexpr std::uint32_t dreamcast_aica_state_contract_version = 2u;

struct DreamcastAicaStateSnapshot {
    std::uint32_t contract_version = dreamcast_aica_state_contract_version;
    AicaRegisterSnapshot registers;
    AicaRtcSnapshot rtc;
    AicaExecutionController::Snapshot execution;
};

class PreparedDreamcastAicaStateRestore final {
  public:
    PreparedDreamcastAicaStateRestore(
        const PreparedDreamcastAicaStateRestore&) = delete;
    PreparedDreamcastAicaStateRestore& operator=(
        const PreparedDreamcastAicaStateRestore&) = delete;
    PreparedDreamcastAicaStateRestore(
        PreparedDreamcastAicaStateRestore&&) noexcept = default;
    PreparedDreamcastAicaStateRestore& operator=(
        PreparedDreamcastAicaStateRestore&&) noexcept = default;

  private:
    PreparedDreamcastAicaStateRestore() = default;
    friend PreparedDreamcastAicaStateRestore
    prepare_dreamcast_aica_state_restore(
        const AicaRegisterFile&,
        const AicaRtc&,
        const AicaExecutionController&,
        DreamcastAicaStateSnapshot,
        std::uint64_t);
    friend void commit_dreamcast_aica_state_restore(
        AicaRegisterFile&,
        AicaRtc&,
        AicaExecutionController&,
        PreparedDreamcastAicaStateRestore) noexcept;

    DreamcastAicaStateSnapshot state_;
};

[[nodiscard]] DreamcastAicaStateSnapshot snapshot_dreamcast_aica_state(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution);
// Normalizes only run-local host observations in a detached capture. Guest
// registers, decoder phase, RTC, timers, interrupts and execution continuation
// remain untouched.
void normalize_dreamcast_aica_observations_for_restore(
    DreamcastAicaStateSnapshot& state,
    ObservationRestorePolicy policy);
void validate_dreamcast_aica_state_restore(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution,
    const DreamcastAicaStateSnapshot& state);
void validate_dreamcast_aica_state_restore(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution,
    const DreamcastAicaStateSnapshot& state,
    std::uint64_t expected_scheduler_cycle);
[[nodiscard]] PreparedDreamcastAicaStateRestore
prepare_dreamcast_aica_state_restore(
    const AicaRegisterFile& registers,
    const AicaRtc& rtc,
    const AicaExecutionController& execution,
    DreamcastAicaStateSnapshot state,
    std::uint64_t expected_scheduler_cycle);
void commit_dreamcast_aica_state_restore(
    AicaRegisterFile& registers,
    AicaRtc& rtc,
    AicaExecutionController& execution,
    PreparedDreamcastAicaStateRestore prepared) noexcept;
void restore_dreamcast_aica_state_passive(
    AicaRegisterFile& registers,
    AicaRtc& rtc,
    AicaExecutionController& execution,
    DreamcastAicaStateSnapshot state);
[[nodiscard]] std::vector<std::uint8_t>
encode_dreamcast_aica_state(const DreamcastAicaStateSnapshot& state);
[[nodiscard]] DreamcastAicaStateSnapshot
decode_dreamcast_aica_state(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory);
[[nodiscard]] std::shared_ptr<AicaRegisterFile>
map_aica_registers(Memory& memory, std::shared_ptr<AicaExecutionController> execution);
[[nodiscard]] std::shared_ptr<AicaRegisterFile>
map_aica_registers(Memory& memory,
                   std::shared_ptr<AicaExecutionController> execution,
                   std::shared_ptr<LinearMemoryDevice> ram);
[[nodiscard]] std::shared_ptr<AicaRtc> map_aica_rtc(Memory& memory, EventScheduler* scheduler);

} // namespace katana::runtime
