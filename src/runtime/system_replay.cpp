#include "katana/runtime/system_replay.hpp"

#include "katana/runtime/scheduler_safepoint.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

bool stable_code(const std::string_view value) noexcept {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](const unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-' ||
               value == '_' || value == '.';
    });
}

bool external_kind(const SystemReplayEventKind kind) noexcept {
    return kind == SystemReplayEventKind::ExternalInput || kind == SystemReplayEventKind::HostEvent;
}

void validate_event(const SystemReplayEvent& event) {
    if (!stable_code(event.code) || external_kind(event.kind) != event.injected) {
        throw std::invalid_argument(
            "Systemereignis braucht portablen Code und explizite externe Injektion.");
    }
}

void hash_byte(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= fnv_prime;
}

void hash_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
        hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void hash_optional(std::uint64_t& hash, const std::optional<std::uint32_t> value) noexcept {
    hash_byte(hash, value.has_value() ? 1u : 0u);
    if (value.has_value()) hash_u64(hash, *value);
}

void hash_event(std::uint64_t& hash, const SystemReplayEvent& event) noexcept {
    hash_u64(hash, event.sequence);
    hash_u64(hash, event.guest_cycle);
    hash_byte(hash, static_cast<std::uint8_t>(event.kind));
    for (const unsigned char value : event.code)
        hash_byte(hash, value);
    hash_byte(hash, 0u);
    hash_optional(hash, event.address);
    hash_optional(hash, event.value);
    hash_u64(hash, event.detail);
    hash_u64(hash, event.auxiliary);
    hash_byte(hash, event.injected ? 1u : 0u);
    hash_u64(hash, event.time_epoch);
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string hex64(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

void optional_hex(std::ostringstream& output, const std::optional<std::uint32_t> value) {
    value.has_value() ? output << '"' << hex32(*value) << '"' : output << "null";
}

const char* safepoint_kind_name(const SafepointKind kind) noexcept {
    switch (kind) {
    case SafepointKind::BlockEnd:
        return "block-end";
    case SafepointKind::LoopBackedge:
        return "loop-backedge";
    case SafepointKind::BeforeDelaySlot:
        return "before-delay-slot";
    case SafepointKind::AfterDelaySlot:
        return "after-delay-slot";
    }
    return "unknown";
}

const char* execution_origin_name(const ExecutionOrigin origin) noexcept {
    return origin == ExecutionOrigin::Backend ? "backend" : "fallback";
}

} // namespace

void SystemReplayLog::record(SystemReplayEvent event) {
    if (final_guest_state_hash_.has_value()) {
        throw std::logic_error("Versiegelter Systemreplay darf keine Ereignisse aufnehmen.");
    }
    validate_event(event);
    if (last_time_epoch_.has_value() &&
        (event.time_epoch < *last_time_epoch_ ||
         (event.time_epoch == *last_time_epoch_ && event.guest_cycle < *last_guest_cycle_))) {
        throw std::invalid_argument("Systemreplay-Gastzeitpaar darf nicht rueckwaerts laufen.");
    }
    if (events_.size() == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Systemreplay-Sequenz ist uebergelaufen.");
    }
    event.sequence = events_.size();
    events_.push_back(std::move(event));
    last_guest_cycle_ = events_.back().guest_cycle;
    last_time_epoch_ = events_.back().time_epoch;
}

bool SystemReplayLog::try_record(SystemReplayEvent event) noexcept {
    try {
        record(std::move(event));
        return true;
    } catch (...) {
        note_dropped_event();
        return false;
    }
}

void SystemReplayLog::note_dropped_event() noexcept {
    if (dropped_events_ != std::numeric_limits<std::uint64_t>::max()) {
        ++dropped_events_;
    }
}

void SystemReplayLog::inject(SystemReplayEvent event) {
    if (!external_kind(event.kind)) {
        throw std::invalid_argument("Nur externe Systemereignisse duerfen injiziert werden.");
    }
    event.injected = true;
    record(std::move(event));
}

void SystemReplayLog::seal(const std::uint64_t final_guest_state_hash) {
    if (final_guest_state_hash_.has_value()) {
        throw std::logic_error("Systemreplay wurde bereits versiegelt.");
    }
    if (dropped_events_ != 0u) {
        throw std::runtime_error("Unvollstaendiger Systemreplay darf nicht versiegelt werden.");
    }
    final_guest_state_hash_ = final_guest_state_hash;
}

bool SystemReplayLog::sealed() const noexcept {
    return final_guest_state_hash_.has_value();
}
const std::vector<SystemReplayEvent>& SystemReplayLog::events() const noexcept {
    return events_;
}
std::uint64_t SystemReplayLog::dropped_events() const noexcept {
    return dropped_events_;
}

std::uint64_t SystemReplayLog::event_hash() const noexcept {
    auto hash = fnv_offset;
    for (const auto& event : events_)
        hash_event(hash, event);
    return hash;
}

std::uint64_t SystemReplayLog::final_guest_state_hash() const {
    if (!final_guest_state_hash_.has_value()) {
        throw std::logic_error("Unversiegelter Systemreplay besitzt keinen Endzustand.");
    }
    return *final_guest_state_hash_;
}

std::string SystemReplayLog::serialize_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-system-replay\",\"report_version\":1"
           << ",\"replay_version\":" << system_replay_schema_version << ",\"status\":\""
           << (dropped_events_ == 0u ? "success" : "failed")
           << "\",\"sealed\":" << (sealed() ? "true" : "false")
           << ",\"dropped_events\":" << dropped_events_ << ",\"event_hash\":\""
           << hex64(event_hash()) << "\",\"final_guest_state_hash\":";
    sealed() ? output << '"' << hex64(*final_guest_state_hash_) << '"' : output << "null";
    output << ",\"events\":[";
    for (std::size_t index = 0u; index < events_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& event = events_[index];
        output << "{\"sequence\":" << event.sequence << ",\"time_epoch\":" << event.time_epoch
               << ",\"guest_cycle\":" << event.guest_cycle << ",\"kind\":\""
               << system_replay_event_kind_name(event.kind) << "\",\"code\":\"" << event.code
               << "\",\"address\":";
        optional_hex(output, event.address);
        output << ",\"value\":";
        optional_hex(output, event.value);
        output << ",\"detail\":" << event.detail << ",\"auxiliary\":" << event.auxiliary
               << ",\"injected\":" << (event.injected ? "true" : "false") << '}';
    }
    output << "]}";
    return output.str();
}

SystemReplayMismatch::SystemReplayMismatch(const std::size_t event_index, std::string reason)
    : std::runtime_error("Systemreplay weicht bei Ereignis " + std::to_string(event_index) +
                         " ab: " + reason),
      event_index_(event_index) {}

std::size_t SystemReplayMismatch::event_index() const noexcept {
    return event_index_;
}

DeterministicSystemReplay::DeterministicSystemReplay(const SystemReplayLog& expected)
    : expected_(expected.events()), expected_guest_state_hash_(expected.final_guest_state_hash()) {
    if (expected.dropped_events() != 0u) {
        throw std::invalid_argument("Unvollstaendige Aufzeichnung kann nicht abgespielt werden.");
    }
}

void DeterministicSystemReplay::observe(SystemReplayEvent event) {
    if (finished_)
        throw std::logic_error("Abgeschlossener Systemreplay kann nicht fortgesetzt werden.");
    validate_event(event);
    if (position_ >= expected_.size()) {
        throw SystemReplayMismatch(position_, "zusaetzliches Ereignis");
    }
    event.sequence = position_;
    if (!(event == expected_[position_])) {
        throw SystemReplayMismatch(position_, "Inhalt oder Reihenfolge unterschiedlich");
    }
    ++position_;
}

void DeterministicSystemReplay::finish(const std::uint64_t final_guest_state_hash) {
    if (finished_) throw std::logic_error("Systemreplay wurde bereits abgeschlossen.");
    if (position_ != expected_.size()) {
        throw SystemReplayMismatch(position_, "erwartetes Ereignis fehlt");
    }
    if (final_guest_state_hash != expected_guest_state_hash_) {
        throw SystemReplayMismatch(position_, "Gastzustandshash unterschiedlich");
    }
    finished_ = true;
}

std::size_t DeterministicSystemReplay::position() const noexcept {
    return position_;
}
bool DeterministicSystemReplay::complete() const noexcept {
    return finished_;
}

SystemReplayEvent make_safepoint_replay_event(const SafepointReport& report) {
    const auto flags = (report.interrupt_delivered ? 1u : 0u) | (report.budget_exhausted ? 2u : 0u);
    return {0u,
            report.delivered_cycle,
            SystemReplayEventKind::CpuSafepoint,
            std::string(safepoint_kind_name(report.kind)) + "-" +
                execution_origin_name(report.origin),
            std::nullopt,
            flags,
            report.requested_cycle,
            report.processed_events,
            false};
}

MemoryAccessObserver system_replay_mmio_observer(SystemReplayLog& log,
                                                 std::function<std::uint64_t()> guest_cycle,
                                                 std::string code) {
    if (!guest_cycle || !stable_code(code)) {
        throw std::invalid_argument("MMIO-Replayobserver braucht Gastzeit und portablen Code.");
    }
    return [&log, cycle = std::move(guest_cycle), event_code = std::move(code)](
               const MemoryAccessEvent& access) noexcept {
        try {
            static_cast<void>(log.try_record({0u,
                                              cycle(),
                                              access.operation == MemoryAccessOperation::Read
                                                  ? SystemReplayEventKind::MmioRead
                                                  : SystemReplayEventKind::MmioWrite,
                                              event_code,
                                              access.address,
                                              access.value,
                                              static_cast<std::uint64_t>(access.width),
                                              0u,
                                              false}));
        } catch (...) {
            log.note_dropped_event();
        }
    };
}

std::uint64_t hash_replay_guest_state(const CpuState& cpu,
                                      const std::uint64_t scheduler_cycle,
                                      const std::uint64_t subsystem_hash) noexcept {
    auto hash = fnv_offset;
    for (const auto value : cpu.r)
        hash_u64(hash, value);
    for (const auto value : cpu.r_bank)
        hash_u64(hash, value);
    for (const auto value : cpu.fr)
        hash_u64(hash, value);
    for (const auto value : cpu.xf)
        hash_u64(hash, value);
    for (const auto value : {cpu.pc,
                             cpu.pr,
                             cpu.gbr,
                             cpu.vbr,
                             cpu.ssr,
                             cpu.spc,
                             cpu.sgr,
                             cpu.dbr,
                             cpu.tra,
                             cpu.tea,
                             cpu.expevt,
                             cpu.intevt,
                             cpu.mach,
                             cpu.macl,
                             cpu.fpul,
                             cpu.read_fpscr(),
                             cpu.read_sr(),
                             cpu.last_prefetch_address})
        hash_u64(hash, value);
    for (const auto value : {cpu.t,
                             cpu.s,
                             cpu.q,
                             cpu.m,
                             cpu.trap_pending,
                             cpu.exception_in_delay_slot,
                             cpu.sleeping,
                             cpu.last_prefetch_was_store_queue})
        hash_byte(hash, value ? 1u : 0u);
    hash_byte(hash, static_cast<std::uint8_t>(cpu.last_exception_cause));
    hash_u64(hash, cpu.prefetch_count);
    hash_u64(hash, scheduler_cycle);
    hash_u64(hash, subsystem_hash);
    return hash;
}

const char* system_replay_event_kind_name(const SystemReplayEventKind kind) noexcept {
    switch (kind) {
    case SystemReplayEventKind::CpuSafepoint:
        return "cpu-safepoint";
    case SystemReplayEventKind::MmioRead:
        return "mmio-read";
    case SystemReplayEventKind::MmioWrite:
        return "mmio-write";
    case SystemReplayEventKind::Dma:
        return "dma";
    case SystemReplayEventKind::Interrupt:
        return "interrupt";
    case SystemReplayEventKind::Timer:
        return "timer";
    case SystemReplayEventKind::SchedulerCallback:
        return "scheduler-callback";
    case SystemReplayEventKind::Video:
        return "video";
    case SystemReplayEventKind::Audio:
        return "audio";
    case SystemReplayEventKind::ExternalInput:
        return "external-input";
    case SystemReplayEventKind::HostEvent:
        return "host-event";
    }
    return "unknown";
}

} // namespace katana::runtime
