#include "katana/runtime/system_replay.hpp"

#include "katana/runtime/scheduler_safepoint.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;
constexpr SystemReplayCoverageMask all_coverage =
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::CpuSafepoint) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::SchedulerCallback) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::AcceptedInterrupt) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Video) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Audio) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Input) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Mmio) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Dma);
constexpr std::string_view ordering_digest_domain = "katana-system-replay-order-v1";

bool stable_code(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= SystemReplayConfig::maximum_code_length &&
           std::all_of(value.begin(), value.end(), [](const unsigned char value) {
               return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
                      value == '-' || value == '_' || value == '.';
           });
}

bool external_kind(const SystemReplayEventKind kind) noexcept {
    return kind == SystemReplayEventKind::ExternalInput || kind == SystemReplayEventKind::HostEvent;
}

bool known_event_kind(const SystemReplayEventKind kind) noexcept {
    switch (kind) {
    case SystemReplayEventKind::CpuSafepoint:
    case SystemReplayEventKind::MmioRead:
    case SystemReplayEventKind::MmioWrite:
    case SystemReplayEventKind::Dma:
    case SystemReplayEventKind::Interrupt:
    case SystemReplayEventKind::Timer:
    case SystemReplayEventKind::SchedulerCallback:
    case SystemReplayEventKind::Video:
    case SystemReplayEventKind::Audio:
    case SystemReplayEventKind::ExternalInput:
    case SystemReplayEventKind::HostEvent:
        return true;
    }
    return false;
}

void validate_event(const SystemReplayEvent& event) {
    if (!known_event_kind(event.kind) || !stable_code(event.code) ||
        external_kind(event.kind) != event.injected) {
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

std::uint64_t ordering_digest_seed(const SystemReplayProfile profile,
                                   const SystemReplayCoverageMask enabled) noexcept {
    auto hash = fnv_offset;
    for (const unsigned char value : ordering_digest_domain)
        hash_byte(hash, value);
    hash_byte(hash, 0u);
    hash_u64(hash, system_replay_schema_version);
    hash_byte(hash, static_cast<std::uint8_t>(profile));
    hash_u64(hash, enabled);
    return hash;
}

std::size_t coverage_index(const SystemReplayCoverageMask coverage) noexcept {
    std::size_t index = 0u;
    auto bit = coverage;
    while (bit > 1u) {
        bit >>= 1u;
        ++index;
    }
    return index;
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string hex64(const std::uint64_t value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
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

class SystemReplayCapacityError final : public std::length_error {
  public:
    SystemReplayCapacityError() : std::length_error("Systemreplay-Kapazitaet ist erschoepft.") {}
};

} // namespace

SystemReplayLog::SystemReplayLog(const SystemReplayConfig config)
    : config_(config), required_coverage_(system_replay_required_coverage(config.profile)) {
    if (config_.profile != SystemReplayProfile::General &&
        config_.profile != SystemReplayProfile::DeterministicV1) {
        throw std::invalid_argument("Systemreplay-Profil ist unbekannt.");
    }
    if (config_.capacity == 0u ||
        config_.capacity > SystemReplayConfig::maximum_capacity) {
        throw std::invalid_argument("Systemreplay-Kapazitaet liegt ausserhalb des Vertrags.");
    }
    events_.reserve(config_.capacity);
}

void SystemReplayLog::enable_coverage(const SystemReplayCoverageMask coverage) {
    if ((coverage & ~all_coverage) != 0u) {
        throw std::invalid_argument("Systemreplay-Coverage enthaelt unbekannte Klassen.");
    }
    if (final_guest_state_hash_.has_value()) {
        throw std::logic_error("Versiegelter Systemreplay darf Coverage nicht aendern.");
    }
    if (!events_.empty() || dropped_events_ != 0u || ordering_digest_initialized_) {
        throw std::logic_error(
            "Systemreplay-Coverage muss vor dem ersten Ereignis aktiviert werden.");
    }
    enabled_coverage_ |= coverage;
}

void SystemReplayLog::record(SystemReplayEvent event) {
    if (final_guest_state_hash_.has_value()) {
        throw std::logic_error("Versiegelter Systemreplay darf keine Ereignisse aufnehmen.");
    }
    validate_event(event);
    const auto coverage = system_replay_event_coverage(event);
    if (config_.profile == SystemReplayProfile::DeterministicV1 &&
        (coverage & ~enabled_coverage_) != 0u) {
        throw std::logic_error(
            "Deterministic-v1-Ereignis besitzt keinen vorab aktivierten Coverage-Hook.");
    }
    if (last_time_epoch_.has_value() &&
        (event.time_epoch < *last_time_epoch_ ||
         (event.time_epoch == *last_time_epoch_ && event.guest_cycle < *last_guest_cycle_))) {
        throw std::invalid_argument("Systemreplay-Gastzeitpaar darf nicht rueckwaerts laufen.");
    }
    if (events_.size() == config_.capacity) {
        note_dropped_event();
        throw SystemReplayCapacityError();
    }
    std::string normalized_code(event.code.data(), event.code.size());
    event.code.swap(normalized_code);
    if (events_.size() == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Systemreplay-Sequenz ist uebergelaufen.");
    }
    event.sequence = events_.size();
    events_.push_back(std::move(event));
    if (!ordering_digest_initialized_) {
        ordering_digest_ = ordering_digest_seed(config_.profile, enabled_coverage_);
        ordering_digest_initialized_ = true;
    }
    hash_event(ordering_digest_, events_.back());
    observed_coverage_ |= coverage;
    for (SystemReplayCoverageMask bit = 1u; bit <= all_coverage; bit <<= 1u) {
        if ((coverage & bit) != 0u) ++event_counts_[coverage_index(bit)];
    }
    last_guest_cycle_ = events_.back().guest_cycle;
    last_time_epoch_ = events_.back().time_epoch;
}

bool SystemReplayLog::try_record(SystemReplayEvent event) noexcept {
    const auto dropped_before = dropped_events_;
    try {
        record(std::move(event));
        return true;
    } catch (...) {
        if (dropped_events_ == dropped_before) note_dropped_event();
        return false;
    }
}

void SystemReplayLog::note_dropped_event() noexcept {
    if (final_guest_state_hash_.has_value()) return;
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
    return ordering_digest();
}

std::uint64_t SystemReplayLog::ordering_digest() const noexcept {
    return ordering_digest_initialized_
               ? ordering_digest_
               : ordering_digest_seed(config_.profile, enabled_coverage_);
}

SystemReplayCoverageMask SystemReplayLog::enabled_coverage() const noexcept {
    return enabled_coverage_;
}

SystemReplayCoverageMask SystemReplayLog::observed_coverage() const noexcept {
    return observed_coverage_;
}

SystemReplayCoverageMask SystemReplayLog::required_coverage() const noexcept {
    return required_coverage_;
}

bool SystemReplayLog::coverage_complete() const noexcept {
    return (required_coverage_ & ~enabled_coverage_) == 0u;
}

const SystemReplayEventCounts& SystemReplayLog::event_counts() const noexcept {
    return event_counts_;
}

std::uint64_t SystemReplayLog::final_guest_state_hash() const {
    if (!final_guest_state_hash_.has_value()) {
        throw std::logic_error("Unversiegelter Systemreplay besitzt keinen Endzustand.");
    }
    return *final_guest_state_hash_;
}

std::string SystemReplayLog::serialize_json() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"schema\":\"katana-system-replay\",\"report_version\":1"
           << ",\"replay_version\":" << system_replay_schema_version << ",\"status\":\""
           << (dropped_events_ == 0u ? "success" : "failed")
           << "\",\"sealed\":" << (sealed() ? "true" : "false")
           << ",\"capacity\":" << config_.capacity
           << ",\"profile\":\"" << system_replay_profile_name(config_.profile) << '"'
           << ",\"enabled_coverage\":" << enabled_coverage_
           << ",\"observed_coverage\":" << observed_coverage_
           << ",\"required_coverage\":" << required_coverage_
           << ",\"coverage_complete\":" << (coverage_complete() ? "true" : "false")
           << ",\"coverage_event_counts\":[";
    for (std::size_t index = 0u; index < event_counts_.size(); ++index) {
        if (index != 0u) output << ',';
        output << event_counts_[index];
    }
    output << ']'
           << ",\"serialize_values\":" << (config_.serialize_values ? "true" : "false")
           << ",\"values_redacted\":" << (config_.serialize_values ? "false" : "true")
           << ",\"codes_redacted\":" << (config_.serialize_values ? "false" : "true")
           << ",\"addresses_redacted\":" << (config_.serialize_values ? "false" : "true")
           << ",\"numeric_payloads_redacted\":"
           << (config_.serialize_values ? "false" : "true")
           << ",\"hashes_redacted\":" << (config_.serialize_values ? "false" : "true")
           << ",\"dropped_events\":" << dropped_events_ << ",\"event_hash\":";
    config_.serialize_values ? output << '"' << hex64(event_hash()) << '"' : output << "null";
    output << ",\"final_guest_state_hash\":";
    sealed() && config_.serialize_values
        ? output << '"' << hex64(*final_guest_state_hash_) << '"'
        : output << "null";
    output << ",\"events\":[";
    for (std::size_t index = 0u; index < events_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& event = events_[index];
        output << "{\"sequence\":" << event.sequence << ",\"time_epoch\":" << event.time_epoch
               << ",\"guest_cycle\":" << event.guest_cycle << ",\"kind\":\""
               << system_replay_event_kind_name(event.kind) << "\",\"code\":";
        config_.serialize_values ? output << '"' << event.code << '"' : output << "null";
        output << ",\"address\":";
        optional_hex(output, config_.serialize_values ? event.address : std::nullopt);
        output << ",\"value\":";
        optional_hex(output, config_.serialize_values ? event.value : std::nullopt);
        output << ",\"detail\":";
        config_.serialize_values ? output << event.detail : output << "null";
        output << ",\"auxiliary\":";
        config_.serialize_values ? output << event.auxiliary : output << "null";
        output << ",\"injected\":" << (event.injected ? "true" : "false") << '}';
    }
    output << "]}";
    return output.str();
}

const SystemReplayConfig& SystemReplayLog::config() const noexcept {
    return config_;
}

SystemReplayMismatch::SystemReplayMismatch(const std::size_t event_index, std::string reason)
    : std::runtime_error("Systemreplay weicht bei Ereignis " + std::to_string(event_index) +
                         " ab: " + reason),
      event_index_(event_index) {}

std::size_t SystemReplayMismatch::event_index() const noexcept {
    return event_index_;
}

DeterministicSystemReplay::DeterministicSystemReplay(const SystemReplayLog& expected) {
    if (expected.dropped_events() != 0u) {
        throw std::invalid_argument("Unvollstaendige Aufzeichnung kann nicht abgespielt werden.");
    }
    if (!expected.sealed()) {
        throw std::invalid_argument("Unversiegelte Aufzeichnung kann nicht abgespielt werden.");
    }
    if (!expected.coverage_complete()) {
        throw std::invalid_argument(
            "Aufzeichnung besitzt nicht alle verpflichtenden Replay-Coverage-Hooks.");
    }
    expected_ = expected.events();
    expected_guest_state_hash_ = expected.final_guest_state_hash();
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
    const auto flags = (report.interrupt_delivered ? 1u : 0u) |
                       (report.budget_exhausted ? 2u : 0u) |
                       (report.guest_cycle_budget_exhausted ? 4u : 0u);
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
                                                 std::string code,
                                                 std::function<std::uint64_t()> time_epoch) {
    if (!guest_cycle || !stable_code(code)) {
        throw std::invalid_argument("MMIO-Replayobserver braucht Gastzeit und portablen Code.");
    }
    return [&log,
            cycle = std::move(guest_cycle),
            epoch = std::move(time_epoch),
            event_code = std::move(code)](const MemoryAccessEvent& access) noexcept {
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
                                              false,
                                              epoch ? epoch() : 0u}));
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

SystemReplayCoverageMask
system_replay_required_coverage(const SystemReplayProfile profile) noexcept {
    switch (profile) {
    case SystemReplayProfile::General:
        return 0u;
    case SystemReplayProfile::DeterministicV1:
        return all_coverage;
    }
    return all_coverage;
}

SystemReplayCoverageMask
system_replay_event_coverage(const SystemReplayEvent& event) noexcept {
    switch (event.kind) {
    case SystemReplayEventKind::CpuSafepoint:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::CpuSafepoint);
    case SystemReplayEventKind::MmioRead:
    case SystemReplayEventKind::MmioWrite:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Mmio);
    case SystemReplayEventKind::Dma:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Dma);
    case SystemReplayEventKind::Interrupt:
        return event.code == "interrupt-accepted"
                   ? static_cast<SystemReplayCoverageMask>(
                         SystemReplayCoverage::AcceptedInterrupt)
                   : 0u;
    case SystemReplayEventKind::SchedulerCallback:
        return static_cast<SystemReplayCoverageMask>(
            SystemReplayCoverage::SchedulerCallback);
    case SystemReplayEventKind::Video:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Video);
    case SystemReplayEventKind::Audio:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Audio);
    case SystemReplayEventKind::ExternalInput:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Input);
    case SystemReplayEventKind::Timer:
    case SystemReplayEventKind::HostEvent:
        return 0u;
    }
    return 0u;
}

const char* system_replay_profile_name(const SystemReplayProfile profile) noexcept {
    switch (profile) {
    case SystemReplayProfile::General:
        return "general";
    case SystemReplayProfile::DeterministicV1:
        return "deterministic-v1";
    }
    return "unknown";
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
