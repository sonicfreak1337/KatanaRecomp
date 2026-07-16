#include "katana/runtime/trace.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {

bool stable_token(const std::string_view value, const bool empty_allowed = true) noexcept {
    if (value.empty()) return empty_allowed;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' || character == '_' ||
               character == '-';
    });
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

void optional_hex(std::ostringstream& output, const std::optional<std::uint32_t> value) {
    value.has_value() ? output << '"' << hex32(*value) << '"' : output << "null";
}

} // namespace

RuntimeTraceRecorder::RuntimeTraceRecorder(const RuntimeTraceConfig config) : config_(config) {
    if (config_.capacity == 0u) {
        throw std::invalid_argument("Runtime-Tracekapazitaet darf nicht null sein.");
    }
    events_.reserve(config_.capacity);
}

void RuntimeTraceRecorder::record(RuntimeTraceEvent event) {
    if (!stable_token(event.code)) {
        throw std::invalid_argument("Runtime-Tracecode ist nicht portabel.");
    }
    if (last_guest_cycle_.has_value() && event.guest_cycle < *last_guest_cycle_) {
        throw std::invalid_argument("Runtime-Tracegastzeit darf nicht rueckwaerts laufen.");
    }
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
        total_events_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Runtime-Tracezaehler ist uebergelaufen.");
    }
    event.sequence = next_sequence_++;
    ++total_events_;
    last_guest_cycle_ = event.guest_cycle;
    if (events_.size() == config_.capacity) {
        if (dropped_events_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Runtime-Tracedropzaehler ist uebergelaufen.");
        }
        ++dropped_events_;
        return;
    }
    if (!config_.capture_memory_values) event.value.reset();
    events_.push_back(std::move(event));
}

void RuntimeTraceRecorder::clear() noexcept {
    events_.clear();
    total_events_ = 0u;
    dropped_events_ = 0u;
    next_sequence_ = 0u;
    last_guest_cycle_.reset();
}

const RuntimeTraceConfig& RuntimeTraceRecorder::config() const noexcept {
    return config_;
}
const std::vector<RuntimeTraceEvent>& RuntimeTraceRecorder::events() const noexcept {
    return events_;
}
std::uint64_t RuntimeTraceRecorder::total_events() const noexcept {
    return total_events_;
}
std::uint64_t RuntimeTraceRecorder::dropped_events() const noexcept {
    return dropped_events_;
}

MemoryAccessObserver
RuntimeTraceRecorder::memory_observer(const RuntimeTraceKind kind,
                                      const RuntimeTraceOrigin origin,
                                      std::function<std::uint64_t()> guest_cycle,
                                      std::function<std::uint32_t()> guest_pc) {
    if ((kind != RuntimeTraceKind::MemoryRead && kind != RuntimeTraceKind::MemoryWrite &&
         kind != RuntimeTraceKind::Watchpoint) ||
        !guest_cycle || !guest_pc) {
        throw std::invalid_argument("Memory-Traceobserver braucht Art und Gastzustandsprovider.");
    }
    return [this, kind, origin, cycle = std::move(guest_cycle), pc = std::move(guest_pc)](
               const MemoryAccessEvent& access) {
        RuntimeTraceKind actual_kind = kind;
        if (kind != RuntimeTraceKind::Watchpoint) {
            actual_kind = access.operation == MemoryAccessOperation::Read
                              ? RuntimeTraceKind::MemoryRead
                              : RuntimeTraceKind::MemoryWrite;
        }
        record({0u,
                cycle(),
                actual_kind,
                origin,
                pc(),
                access.address,
                access.width,
                access.value,
                kind == RuntimeTraceKind::Watchpoint ? "watchpoint-hit" : "memory-access"});
    };
}

std::string RuntimeTraceRecorder::serialize_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-runtime-trace\",\"report_version\":1"
           << ",\"trace_version\":" << runtime_trace_schema_version
           << ",\"status\":\"success\",\"capacity\":" << config_.capacity
           << ",\"capture_memory_values\":" << (config_.capture_memory_values ? "true" : "false")
           << ",\"total_events\":" << total_events_ << ",\"dropped_events\":" << dropped_events_
           << ",\"events\":[";
    for (std::size_t index = 0u; index < events_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& event = events_[index];
        output << "{\"sequence\":" << event.sequence << ",\"guest_cycle\":" << event.guest_cycle
               << ",\"kind\":\"" << runtime_trace_kind_name(event.kind) << "\",\"origin\":\""
               << runtime_trace_origin_name(event.origin) << "\",\"pc\":\"" << hex32(event.pc)
               << "\",\"address\":";
        optional_hex(output, event.address);
        output << ",\"width\":";
        event.width.has_value() ? output << static_cast<unsigned>(*event.width) : output << "null";
        output << ",\"value\":";
        optional_hex(output, event.value);
        output << ",\"code\":\"" << event.code << "\"}";
    }
    output << "]}\n";
    return output.str();
}

const char* runtime_trace_kind_name(const RuntimeTraceKind kind) noexcept {
    switch (kind) {
    case RuntimeTraceKind::IrInstruction:
        return "ir-instruction";
    case RuntimeTraceKind::BlockEntry:
        return "block-entry";
    case RuntimeTraceKind::BlockExit:
        return "block-exit";
    case RuntimeTraceKind::MemoryRead:
        return "memory-read";
    case RuntimeTraceKind::MemoryWrite:
        return "memory-write";
    case RuntimeTraceKind::Watchpoint:
        return "watchpoint";
    case RuntimeTraceKind::Exception:
        return "exception";
    case RuntimeTraceKind::Scheduler:
        return "scheduler";
    }
    return "unknown";
}

const char* runtime_trace_origin_name(const RuntimeTraceOrigin origin) noexcept {
    switch (origin) {
    case RuntimeTraceOrigin::Backend:
        return "backend";
    case RuntimeTraceOrigin::Fallback:
        return "fallback";
    case RuntimeTraceOrigin::Runtime:
        return "runtime";
    case RuntimeTraceOrigin::External:
        return "external";
    }
    return "unknown";
}

} // namespace katana::runtime
