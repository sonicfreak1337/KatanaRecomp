#pragma once

#include "katana/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t runtime_trace_schema_version = 1u;

enum class RuntimeTraceKind : std::uint8_t {
    IrInstruction,
    BlockEntry,
    BlockExit,
    MemoryRead,
    MemoryWrite,
    Watchpoint,
    Exception,
    Scheduler
};

enum class RuntimeTraceOrigin : std::uint8_t { Backend, Fallback, Runtime, External };

struct RuntimeTraceEvent {
    std::uint64_t sequence = 0u;
    std::uint64_t guest_cycle = 0u;
    RuntimeTraceKind kind = RuntimeTraceKind::IrInstruction;
    RuntimeTraceOrigin origin = RuntimeTraceOrigin::Runtime;
    std::uint32_t pc = 0u;
    std::optional<std::uint32_t> address;
    std::optional<MemoryAccessWidth> width;
    std::optional<std::uint32_t> value;
    std::string code;
};

struct RuntimeTraceConfig {
    std::size_t capacity = 4096u;
    bool capture_memory_values = false;
};

class RuntimeTraceRecorder final {
  public:
    explicit RuntimeTraceRecorder(RuntimeTraceConfig config = {});

    void record(RuntimeTraceEvent event);
    void clear() noexcept;
    [[nodiscard]] const RuntimeTraceConfig& config() const noexcept;
    [[nodiscard]] const std::vector<RuntimeTraceEvent>& events() const noexcept;
    [[nodiscard]] std::uint64_t total_events() const noexcept;
    [[nodiscard]] std::uint64_t dropped_events() const noexcept;
    [[nodiscard]] std::string serialize_json() const;

    [[nodiscard]] MemoryAccessObserver memory_observer(RuntimeTraceKind kind,
                                                       RuntimeTraceOrigin origin,
                                                       std::function<std::uint64_t()> guest_cycle,
                                                       std::function<std::uint32_t()> guest_pc);

  private:
    RuntimeTraceConfig config_;
    std::vector<RuntimeTraceEvent> events_;
    std::uint64_t total_events_ = 0u;
    std::uint64_t dropped_events_ = 0u;
    std::uint64_t next_sequence_ = 0u;
    std::optional<std::uint64_t> last_guest_cycle_;
};

[[nodiscard]] const char* runtime_trace_kind_name(RuntimeTraceKind kind) noexcept;
[[nodiscard]] const char* runtime_trace_origin_name(RuntimeTraceOrigin origin) noexcept;

} // namespace katana::runtime
