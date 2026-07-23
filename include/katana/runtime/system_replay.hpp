#pragma once

#include "katana/runtime/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t system_replay_schema_version = 4u;
inline constexpr std::string_view runtime_probe_checkpoint_line_prefix =
    "KATANA_RUNTIME_PROBE_CHECKPOINT ";

class IndirectDispatchMetrics;
class EventScheduler;
enum class RuntimeDispatchClass : std::uint8_t;
enum class SchedulerEventKind : std::uint32_t;
struct SafepointReport;

enum class SystemReplayEventKind : std::uint8_t {
    CpuSafepoint,
    MmioRead,
    MmioWrite,
    Dma,
    Interrupt,
    Timer,
    SchedulerCallback,
    Video,
    Audio,
    ExternalInput,
    HostEvent,
    BlockDispatchHit,
    BlockDispatchMiss,
    ControlledFallback,
    GuestException,
    GuestCheckpoint
};

enum class SystemReplayProfile : std::uint8_t {
    General,
    DeterministicV1
};

enum class SystemReplayCoverage : std::uint32_t {
    None = 0u,
    CpuSafepoint = 1u << 0u,
    SchedulerCallback = 1u << 1u,
    AcceptedInterrupt = 1u << 2u,
    Video = 1u << 3u,
    Audio = 1u << 4u,
    Input = 1u << 5u,
    Mmio = 1u << 6u,
    Dma = 1u << 7u,
    BlockDispatch = 1u << 8u,
    GuestException = 1u << 9u,
    ControlledFallback = 1u << 10u,
    GuestCheckpoint = 1u << 11u
};

using SystemReplayCoverageMask = std::uint32_t;
inline constexpr std::size_t system_replay_coverage_class_count = 12u;
using SystemReplayEventCounts =
    std::array<std::uint64_t, system_replay_coverage_class_count>;

enum class SystemReplayCheckpointKind : std::uint8_t {
    RuntimeStarted = 1u,
    GuestProgramEntered = 2u,
    FirstGuestFrame = 3u,
    GuestInputInteractive = 4u,
    ControlledRetailScene = 5u
};

struct SystemReplayCheckpoint {
    std::uint64_t sequence = 0u;
    SystemReplayCheckpointKind kind = SystemReplayCheckpointKind::RuntimeStarted;

    [[nodiscard]] bool operator==(const SystemReplayCheckpoint&) const = default;
};

struct SystemReplayEvent {
    std::uint64_t sequence = 0u;
    std::uint64_t guest_cycle = 0u;
    SystemReplayEventKind kind = SystemReplayEventKind::CpuSafepoint;
    std::string code;
    std::optional<std::uint32_t> address;
    std::optional<std::uint32_t> value;
    std::uint64_t detail = 0u;
    std::uint64_t auxiliary = 0u;
    bool injected = false;
    std::uint64_t time_epoch = 0u;

    [[nodiscard]] bool operator==(const SystemReplayEvent&) const = default;
};

struct SystemReplayConfig {
    static constexpr std::size_t default_capacity = 4096u;
    static constexpr std::size_t maximum_capacity = 65'536u;
    static constexpr std::size_t maximum_code_length = 64u;

    std::size_t capacity = default_capacity;
    bool serialize_values = false;
    SystemReplayProfile profile = SystemReplayProfile::General;
};

class SystemReplayLog final {
  public:
    explicit SystemReplayLog(SystemReplayConfig config = {});
    void enable_coverage(SystemReplayCoverageMask coverage);
    void record(SystemReplayEvent event);
    [[nodiscard]] bool try_record(SystemReplayEvent event) noexcept;
    void note_dropped_event() noexcept;
    void inject(SystemReplayEvent event);
    void seal(std::uint64_t final_guest_state_hash);
    [[nodiscard]] bool sealed() const noexcept;
    [[nodiscard]] const std::vector<SystemReplayEvent>& events() const noexcept;
    [[nodiscard]] std::uint64_t dropped_events() const noexcept;
    [[nodiscard]] std::uint64_t event_hash() const noexcept;
    [[nodiscard]] std::uint64_t ordering_digest() const noexcept;
    [[nodiscard]] SystemReplayCoverageMask enabled_coverage() const noexcept;
    [[nodiscard]] SystemReplayCoverageMask observed_coverage() const noexcept;
    [[nodiscard]] SystemReplayCoverageMask required_coverage() const noexcept;
    [[nodiscard]] bool coverage_complete() const noexcept;
    [[nodiscard]] const SystemReplayEventCounts& event_counts() const noexcept;
    [[nodiscard]] std::uint64_t final_guest_state_hash() const;
    [[nodiscard]] std::string serialize_json() const;
    [[nodiscard]] const SystemReplayConfig& config() const noexcept;

  private:
    SystemReplayConfig config_;
    std::vector<SystemReplayEvent> events_;
    std::optional<std::uint64_t> last_guest_cycle_;
    std::optional<std::uint64_t> last_time_epoch_;
    std::optional<std::uint64_t> final_guest_state_hash_;
    std::uint64_t dropped_events_ = 0u;
    std::uint64_t ordering_digest_ = 0u;
    SystemReplayCoverageMask enabled_coverage_ = 0u;
    SystemReplayCoverageMask observed_coverage_ = 0u;
    SystemReplayCoverageMask required_coverage_ = 0u;
    SystemReplayEventCounts event_counts_{};
    bool ordering_digest_initialized_ = false;
};

class SystemReplayMismatch final : public std::runtime_error {
  public:
    SystemReplayMismatch(std::size_t event_index, std::string reason);
    [[nodiscard]] std::size_t event_index() const noexcept;

  private:
    std::size_t event_index_ = 0u;
};

class DeterministicSystemReplay final {
  public:
    explicit DeterministicSystemReplay(const SystemReplayLog& expected);
    void observe(SystemReplayEvent event);
    void finish(std::uint64_t final_guest_state_hash);
    [[nodiscard]] std::size_t position() const noexcept;
    [[nodiscard]] bool complete() const noexcept;

  private:
    std::vector<SystemReplayEvent> expected_;
    std::uint64_t expected_guest_state_hash_ = 0u;
    std::size_t position_ = 0u;
    bool finished_ = false;
};

class SystemReplayObservationSession final {
  public:
    explicit SystemReplayObservationSession(SystemReplayLog* replay_log = nullptr,
                                            const EventScheduler* scheduler = nullptr);

    void observe_block_dispatch_hit(RuntimeDispatchClass dispatch_class,
                                    bool materialized = false) noexcept;
    void observe_block_dispatch_miss(const IndirectDispatchMetrics& metrics) noexcept;
    void observe_controlled_fallback() noexcept;
    void observe_guest_exception(ExceptionCause cause) noexcept;
    [[nodiscard]] bool
    observe_guest_checkpoint(SystemReplayCheckpointKind checkpoint) noexcept;
    [[nodiscard]] const std::optional<SystemReplayCheckpoint>&
    last_checkpoint() const noexcept;
    [[nodiscard]] std::string serialize_checkpoint_json() const;

  private:
    void record(SystemReplayEventKind kind,
                std::uint64_t guest_cycle,
                std::uint64_t time_epoch,
                std::string code,
                std::uint64_t detail = 0u,
                std::uint64_t auxiliary = 0u) noexcept;

    SystemReplayLog* replay_log_ = nullptr;
    const EventScheduler* scheduler_ = nullptr;
    std::optional<SystemReplayCheckpoint> last_checkpoint_;
    std::uint64_t dispatch_hit_count_ = 0u;
    std::uint64_t controlled_fallback_count_ = 0u;
    std::uint64_t guest_exception_count_ = 0u;
};

[[nodiscard]] SystemReplayEvent make_safepoint_replay_event(const SafepointReport& report);
[[nodiscard]] MemoryAccessObserver
system_replay_mmio_observer(SystemReplayLog& log,
                            std::function<std::uint64_t()> guest_cycle,
                            std::string code,
                            std::function<std::uint64_t()> time_epoch = {});
[[nodiscard]] std::uint64_t hash_replay_guest_state(const CpuState& cpu,
                                                    std::uint64_t scheduler_cycle,
                                                    std::uint64_t subsystem_hash = 0u) noexcept;
[[nodiscard]] SystemReplayCoverageMask
system_replay_required_coverage(SystemReplayProfile profile) noexcept;
[[nodiscard]] SystemReplayCoverageMask
system_replay_event_coverage(const SystemReplayEvent& event) noexcept;
[[nodiscard]] const char* system_replay_profile_name(SystemReplayProfile profile) noexcept;
[[nodiscard]] const char* system_replay_event_kind_name(SystemReplayEventKind kind) noexcept;
[[nodiscard]] const char*
system_replay_checkpoint_kind_name(SystemReplayCheckpointKind checkpoint) noexcept;
[[nodiscard]] const char*
system_replay_scheduler_event_code(SchedulerEventKind kind) noexcept;

} // namespace katana::runtime
