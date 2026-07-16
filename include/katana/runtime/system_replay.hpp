#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t system_replay_schema_version = 1u;

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
    HostEvent
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

class SystemReplayLog final {
  public:
    void record(SystemReplayEvent event);
    [[nodiscard]] bool try_record(SystemReplayEvent event) noexcept;
    void note_dropped_event() noexcept;
    void inject(SystemReplayEvent event);
    void seal(std::uint64_t final_guest_state_hash);
    [[nodiscard]] bool sealed() const noexcept;
    [[nodiscard]] const std::vector<SystemReplayEvent>& events() const noexcept;
    [[nodiscard]] std::uint64_t dropped_events() const noexcept;
    [[nodiscard]] std::uint64_t event_hash() const noexcept;
    [[nodiscard]] std::uint64_t final_guest_state_hash() const;
    [[nodiscard]] std::string serialize_json() const;

  private:
    std::vector<SystemReplayEvent> events_;
    std::optional<std::uint64_t> last_guest_cycle_;
    std::optional<std::uint64_t> last_time_epoch_;
    std::optional<std::uint64_t> final_guest_state_hash_;
    std::uint64_t dropped_events_ = 0u;
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

[[nodiscard]] SystemReplayEvent make_safepoint_replay_event(const SafepointReport& report);
[[nodiscard]] MemoryAccessObserver
system_replay_mmio_observer(SystemReplayLog& log,
                            std::function<std::uint64_t()> guest_cycle,
                            std::string code,
                            std::function<std::uint64_t()> time_epoch = {});
[[nodiscard]] std::uint64_t hash_replay_guest_state(const CpuState& cpu,
                                                    std::uint64_t scheduler_cycle,
                                                    std::uint64_t subsystem_hash = 0u) noexcept;
[[nodiscard]] const char* system_replay_event_kind_name(SystemReplayEventKind kind) noexcept;

} // namespace katana::runtime
