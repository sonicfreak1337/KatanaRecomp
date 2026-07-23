#include "katana/runtime/system_replay.hpp"

#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/scheduler.hpp"
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
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Dma) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::BlockDispatch) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestException) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::ControlledFallback) |
    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestCheckpoint);
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

bool known_storage_mode(const SystemReplayStorageMode mode) noexcept {
    return mode == SystemReplayStorageMode::ExactEvents ||
           mode == SystemReplayStorageMode::DigestStream;
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
    case SystemReplayEventKind::BlockDispatchHit:
    case SystemReplayEventKind::BlockDispatchMiss:
    case SystemReplayEventKind::ControlledFallback:
    case SystemReplayEventKind::GuestException:
    case SystemReplayEventKind::GuestCheckpoint:
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

std::size_t mismatch_index(const std::uint64_t value) noexcept {
    return value > std::numeric_limits<std::size_t>::max()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(value);
}

bool advance_power_of_two_sample(std::uint64_t& count) noexcept {
    if (count != std::numeric_limits<std::uint64_t>::max()) ++count;
    return count != 0u && (count & (count - 1u)) == 0u;
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
    if (!known_storage_mode(config_.storage_mode)) {
        throw std::invalid_argument("Systemreplay-Speichermodus ist unbekannt.");
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
    if (event_count_ != 0u || dropped_events_ != 0u || ordering_digest_initialized_) {
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
    if (config_.storage_mode == SystemReplayStorageMode::ExactEvents &&
        events_.size() == config_.capacity) {
        note_dropped_event();
        throw SystemReplayCapacityError();
    }
    if (event_count_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Systemreplay-Sequenz ist uebergelaufen.");
    }
    for (SystemReplayCoverageMask bit = 1u; bit <= all_coverage; bit <<= 1u) {
        if ((coverage & bit) != 0u &&
            event_counts_[coverage_index(bit)] ==
                std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Systemreplay-Ereigniszaehler ist uebergelaufen.");
        }
    }
    event.sequence = event_count_;
    const auto retain_event =
        config_.storage_mode == SystemReplayStorageMode::ExactEvents ||
        events_.size() < config_.capacity;
    if (retain_event) {
        std::string normalized_code(event.code.data(), event.code.size());
        event.code.swap(normalized_code);
    }
    const SystemReplayEvent* recorded_event = &event;
    if (retain_event) {
        events_.push_back(std::move(event));
        recorded_event = &events_.back();
    }
    if (!ordering_digest_initialized_) {
        ordering_digest_ = ordering_digest_seed(config_.profile, enabled_coverage_);
        ordering_digest_initialized_ = true;
    }
    hash_event(ordering_digest_, *recorded_event);
    observed_coverage_ |= coverage;
    for (SystemReplayCoverageMask bit = 1u; bit <= all_coverage; bit <<= 1u) {
        if ((coverage & bit) != 0u) ++event_counts_[coverage_index(bit)];
    }
    last_guest_cycle_ = recorded_event->guest_cycle;
    last_time_epoch_ = recorded_event->time_epoch;
    ++event_count_;
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
std::uint64_t SystemReplayLog::event_count() const noexcept {
    return event_count_;
}
std::uint64_t SystemReplayLog::summarized_event_count() const noexcept {
    return event_count_ - static_cast<std::uint64_t>(events_.size());
}
bool SystemReplayLog::exact_event_stream_available() const noexcept {
    return dropped_events_ == 0u && summarized_event_count() == 0u;
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
           << ",\"storage_mode\":\""
           << system_replay_storage_mode_name(config_.storage_mode) << '"'
           << ",\"event_count\":" << event_count_
           << ",\"retained_event_count\":" << events_.size()
           << ",\"summarized_event_count\":" << summarized_event_count()
           << ",\"exact_event_stream\":"
           << (exact_event_stream_available() ? "true" : "false")
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
    if (expected.config().storage_mode != SystemReplayStorageMode::ExactEvents ||
        !expected.exact_event_stream_available()) {
        throw std::invalid_argument(
            "Digest-Aufzeichnung braucht den typisierten Digest-Replay.");
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

DeterministicSystemReplayDigest::DeterministicSystemReplayDigest(
    const SystemReplayLog& expected)
    : observed_({expected.config().capacity,
                 false,
                 expected.config().profile,
                 SystemReplayStorageMode::DigestStream}) {
    if (expected.config().storage_mode != SystemReplayStorageMode::DigestStream) {
        throw std::invalid_argument(
            "Digest-Replay braucht eine explizite Digest-Stream-Aufzeichnung.");
    }
    if (expected.dropped_events() != 0u) {
        throw std::invalid_argument(
            "Unvollstaendige Digest-Aufzeichnung kann nicht verifiziert werden.");
    }
    if (!expected.sealed()) {
        throw std::invalid_argument(
            "Unversiegelte Digest-Aufzeichnung kann nicht verifiziert werden.");
    }
    if (!expected.coverage_complete()) {
        throw std::invalid_argument(
            "Digest-Aufzeichnung besitzt nicht alle verpflichtenden Replay-Coverage-Hooks.");
    }
    observed_.enable_coverage(expected.enabled_coverage());
    expected_witnesses_ = expected.events();
    expected_event_count_ = expected.event_count();
    expected_ordering_digest_ = expected.ordering_digest();
    expected_guest_state_hash_ = expected.final_guest_state_hash();
    expected_observed_coverage_ = expected.observed_coverage();
    expected_event_counts_ = expected.event_counts();
}

void DeterministicSystemReplayDigest::observe(SystemReplayEvent event) {
    if (finished_) {
        throw std::logic_error(
            "Abgeschlossener Systemreplay-Digest kann nicht fortgesetzt werden.");
    }
    const auto event_index = observed_.event_count();
    if (event_index >= expected_event_count_) {
        throw SystemReplayMismatch(mismatch_index(event_index), "zusaetzliches Ereignis");
    }
    observed_.record(std::move(event));
    if (event_index < expected_witnesses_.size() &&
        observed_.events().back() != expected_witnesses_[event_index]) {
        throw SystemReplayMismatch(
            mismatch_index(event_index),
            "Inhalt oder Reihenfolge im gespeicherten Digest-Witness unterschiedlich");
    }
}

void DeterministicSystemReplayDigest::finish(
    const std::uint64_t final_guest_state_hash) {
    if (finished_) {
        throw std::logic_error("Systemreplay-Digest wurde bereits abgeschlossen.");
    }
    if (observed_.event_count() != expected_event_count_) {
        throw SystemReplayMismatch(mismatch_index(observed_.event_count()),
                                   "erwartetes Ereignis fehlt");
    }
    observed_.seal(final_guest_state_hash);
    if (observed_.ordering_digest() != expected_ordering_digest_ ||
        observed_.observed_coverage() != expected_observed_coverage_ ||
        observed_.event_counts() != expected_event_counts_) {
        throw SystemReplayMismatch(
            mismatch_index(observed_.event_count()),
            "Digest, Coverage oder Ereigniszaehler unterschiedlich");
    }
    if (final_guest_state_hash != expected_guest_state_hash_) {
        throw SystemReplayMismatch(mismatch_index(observed_.event_count()),
                                   "Gastzustandshash unterschiedlich");
    }
    finished_ = true;
}

std::uint64_t DeterministicSystemReplayDigest::position() const noexcept {
    return observed_.event_count();
}

bool DeterministicSystemReplayDigest::complete() const noexcept {
    return finished_;
}

SystemReplayObservationSession::SystemReplayObservationSession(
    SystemReplayLog* replay_log,
    const EventScheduler* scheduler)
    : replay_log_(replay_log), scheduler_(scheduler) {
    if (replay_log_ != nullptr && scheduler_ == nullptr) {
        throw std::invalid_argument(
            "Systemreplay-Observation braucht eine logische Scheduleruhr.");
    }
    if (replay_log_ != nullptr) {
        replay_log_->enable_coverage(
            static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::BlockDispatch) |
            static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestException) |
            static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::ControlledFallback) |
            static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestCheckpoint));
    }
}

void SystemReplayObservationSession::record(const SystemReplayEventKind kind,
                                            const std::uint64_t guest_cycle,
                                            const std::uint64_t time_epoch,
                                            std::string code,
                                            const std::uint64_t detail,
                                            const std::uint64_t auxiliary) noexcept {
    if (replay_log_ == nullptr) return;
    static_cast<void>(replay_log_->try_record({0u,
                                               guest_cycle,
                                               kind,
                                               std::move(code),
                                               std::nullopt,
                                               std::nullopt,
                                               detail,
                                               auxiliary,
                                               false,
                                               time_epoch}));
}

void SystemReplayObservationSession::observe_block_dispatch_hit(
    const RuntimeDispatchClass dispatch_class,
    const bool materialized) noexcept {
    const auto sampled = advance_power_of_two_sample(dispatch_hit_count_);
    if (!materialized && !sampled) return;
    record(SystemReplayEventKind::BlockDispatchHit,
           scheduler_ != nullptr ? scheduler_->current_cycle() : 0u,
           scheduler_ != nullptr ? scheduler_->reset_generation() : 0u,
           materialized ? "block-dispatch-hit-materialized" : "block-dispatch-hit",
           static_cast<std::uint64_t>(dispatch_class),
           dispatch_hit_count_);
}

void SystemReplayObservationSession::observe_block_dispatch_miss(
    const IndirectDispatchMetrics& metrics) noexcept {
    const auto& first_error = metrics.first_error();
    record(SystemReplayEventKind::BlockDispatchMiss,
           scheduler_ != nullptr ? scheduler_->current_cycle() : 0u,
           scheduler_ != nullptr ? scheduler_->reset_generation() : 0u,
           "block-dispatch-miss",
           first_error ? static_cast<std::uint64_t>(first_error->error) : 0u,
           first_error ? static_cast<std::uint64_t>(first_error->dispatch_class) : 0u);
}

void SystemReplayObservationSession::observe_controlled_fallback() noexcept {
    if (!advance_power_of_two_sample(controlled_fallback_count_)) return;
    record(SystemReplayEventKind::ControlledFallback,
           scheduler_ != nullptr ? scheduler_->current_cycle() : 0u,
           scheduler_ != nullptr ? scheduler_->reset_generation() : 0u,
           "controlled-fallback",
           0u,
           controlled_fallback_count_);
}

void SystemReplayObservationSession::observe_guest_exception(
    const ExceptionCause cause) noexcept {
    if (cause == ExceptionCause::None) return;
    if (!advance_power_of_two_sample(guest_exception_count_)) return;
    record(SystemReplayEventKind::GuestException,
           scheduler_ != nullptr ? scheduler_->current_cycle() : 0u,
           scheduler_ != nullptr ? scheduler_->reset_generation() : 0u,
           "guest-exception",
           static_cast<std::uint64_t>(cause),
           guest_exception_count_);
}

bool SystemReplayObservationSession::observe_guest_checkpoint(
    const SystemReplayCheckpointKind checkpoint) noexcept {
    const auto ordinal = static_cast<std::uint8_t>(checkpoint);
    if (ordinal < static_cast<std::uint8_t>(SystemReplayCheckpointKind::RuntimeStarted) ||
        ordinal >
            static_cast<std::uint8_t>(SystemReplayCheckpointKind::ControlledRetailScene) ||
        (last_checkpoint_.has_value() &&
         ordinal <= static_cast<std::uint8_t>(last_checkpoint_->kind)) ||
        (last_checkpoint_.has_value() &&
         last_checkpoint_->sequence == std::numeric_limits<std::uint64_t>::max())) {
        return false;
    }
    const auto sequence = last_checkpoint_.has_value() ? last_checkpoint_->sequence + 1u : 1u;
    last_checkpoint_ = SystemReplayCheckpoint{sequence, checkpoint};
    record(SystemReplayEventKind::GuestCheckpoint,
           scheduler_ != nullptr ? scheduler_->current_cycle() : 0u,
           scheduler_ != nullptr ? scheduler_->reset_generation() : 0u,
           "guest-checkpoint",
           ordinal,
           sequence);
    return true;
}

const std::optional<SystemReplayCheckpoint>&
SystemReplayObservationSession::last_checkpoint() const noexcept {
    return last_checkpoint_;
}

std::string SystemReplayObservationSession::serialize_checkpoint_json() const {
    if (!last_checkpoint_.has_value()) {
        throw std::logic_error("Observation-Session besitzt keinen Gastcheckpoint.");
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"schema\":\"katana.runtime-probe-checkpoint\",\"report_version\":1"
           << ",\"status\":\"observed\",\"sequence\":" << last_checkpoint_->sequence
           << ",\"checkpoint\":\""
           << system_replay_checkpoint_kind_name(last_checkpoint_->kind) << "\"}";
    return output.str();
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
    hash_u64(hash, cpu.exception_generation);
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
    case SystemReplayEventKind::BlockDispatchHit:
    case SystemReplayEventKind::BlockDispatchMiss:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::BlockDispatch);
    case SystemReplayEventKind::GuestException:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestException);
    case SystemReplayEventKind::ControlledFallback:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::ControlledFallback);
    case SystemReplayEventKind::GuestCheckpoint:
        return static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestCheckpoint);
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

const char*
system_replay_storage_mode_name(const SystemReplayStorageMode mode) noexcept {
    switch (mode) {
    case SystemReplayStorageMode::ExactEvents:
        return "exact-events";
    case SystemReplayStorageMode::DigestStream:
        return "digest-stream";
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
    case SystemReplayEventKind::BlockDispatchHit:
        return "block-dispatch-hit";
    case SystemReplayEventKind::BlockDispatchMiss:
        return "block-dispatch-miss";
    case SystemReplayEventKind::ControlledFallback:
        return "controlled-fallback";
    case SystemReplayEventKind::GuestException:
        return "guest-exception";
    case SystemReplayEventKind::GuestCheckpoint:
        return "guest-checkpoint";
    }
    return "unknown";
}

const char*
system_replay_checkpoint_kind_name(const SystemReplayCheckpointKind checkpoint) noexcept {
    switch (checkpoint) {
    case SystemReplayCheckpointKind::RuntimeStarted:
        return "runtime-started";
    case SystemReplayCheckpointKind::GuestProgramEntered:
        return "guest-program-entered";
    case SystemReplayCheckpointKind::FirstGuestFrame:
        return "first-guest-frame";
    case SystemReplayCheckpointKind::GuestInputInteractive:
        return "guest-input-interactive";
    case SystemReplayCheckpointKind::ControlledRetailScene:
        return "controlled-retail-scene";
    }
    return "unknown";
}

const char* system_replay_scheduler_event_code(const SchedulerEventKind kind) noexcept {
    switch (kind) {
    case SchedulerEventKind::Unknown:
        return "scheduler-unknown";
    case SchedulerEventKind::DiscRead:
        return "gdrom-disc-read";
    case SchedulerEventKind::Sh4Dmac:
        return "sh4-dmac";
    case SchedulerEventKind::GdRomPacket:
        return "gdrom-packet";
    case SchedulerEventKind::HollyG1Dma:
        return "holly-g1-dma";
    case SchedulerEventKind::HollyG2Dma:
        return "holly-g2-dma";
    case SchedulerEventKind::HollyPvrDma:
        return "holly-pvr-dma";
    case SchedulerEventKind::MapleDma:
        return "maple-dma";
    case SchedulerEventKind::MediaVideo:
        return "media-video";
    case SchedulerEventKind::MediaAudio:
        return "media-audio";
    case SchedulerEventKind::PvrRender:
        return "pvr-render";
    case SchedulerEventKind::PvrVblankIn:
        return "pvr-vblank-in";
    case SchedulerEventKind::PvrVblankOut:
        return "pvr-vblank-out";
    case SchedulerEventKind::PvrHblank:
        return "pvr-hblank";
    case SchedulerEventKind::ScifTransmit:
        return "scif-transmit";
    case SchedulerEventKind::SystemAsic:
        return "system-asic";
    case SchedulerEventKind::Sh4Rtc:
        return "sh4-rtc";
    case SchedulerEventKind::Sh4Tmu0:
        return "sh4-tmu0";
    case SchedulerEventKind::Sh4Tmu1:
        return "sh4-tmu1";
    case SchedulerEventKind::Sh4Tmu2:
        return "sh4-tmu2";
    case SchedulerEventKind::AicaTick:
        return "aica-tick";
    }
    return "scheduler-unknown";
}

} // namespace katana::runtime
